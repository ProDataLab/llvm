//===- llvm/CodeGen/AsmPrinter/AccelTable.cpp - Accelerator Tables --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains support for writing accelerator tables.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/AccelTable.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/DIE.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using namespace llvm;

void AppleAccelTableHeader::emit(AsmPrinter *Asm) {
  // Emit Header.
  Asm->OutStreamer->AddComment("Header Magic");
  Asm->EmitInt32(Header.Magic);
  Asm->OutStreamer->AddComment("Header Version");
  Asm->EmitInt16(Header.Version);
  Asm->OutStreamer->AddComment("Header Hash Function");
  Asm->EmitInt16(Header.HashFunction);
  Asm->OutStreamer->AddComment("Header Bucket Count");
  Asm->EmitInt32(Header.BucketCount);
  Asm->OutStreamer->AddComment("Header Hash Count");
  Asm->EmitInt32(Header.HashCount);
  Asm->OutStreamer->AddComment("Header Data Length");
  Asm->EmitInt32(Header.HeaderDataLength);

  //  Emit Header Data
  Asm->OutStreamer->AddComment("HeaderData Die Offset Base");
  Asm->EmitInt32(HeaderData.DieOffsetBase);
  Asm->OutStreamer->AddComment("HeaderData Atom Count");
  Asm->EmitInt32(HeaderData.Atoms.size());

  for (size_t i = 0; i < HeaderData.Atoms.size(); i++) {
    Atom A = HeaderData.Atoms[i];
    Asm->OutStreamer->AddComment(dwarf::AtomTypeString(A.Type));
    Asm->EmitInt16(A.Type);
    Asm->OutStreamer->AddComment(dwarf::FormEncodingString(A.Form));
    Asm->EmitInt16(A.Form);
  }
}

void AppleAccelTableHeader::setBucketAndHashCount(uint32_t HashCount) {
  if (HashCount > 1024)
    Header.BucketCount = HashCount / 4;
  else if (HashCount > 16)
    Header.BucketCount = HashCount / 2;
  else
    Header.BucketCount = HashCount > 0 ? HashCount : 1;

  Header.HashCount = HashCount;
}

void AppleAccelTableBase::emitHeader(AsmPrinter *Asm) { Header.emit(Asm); }

void AppleAccelTableBase::emitBuckets(AsmPrinter *Asm) {
  unsigned index = 0;
  for (size_t i = 0, e = Buckets.size(); i < e; ++i) {
    Asm->OutStreamer->AddComment("Bucket " + Twine(i));
    if (!Buckets[i].empty())
      Asm->EmitInt32(index);
    else
      Asm->EmitInt32(std::numeric_limits<uint32_t>::max());
    // Buckets point in the list of hashes, not to the data. Do not increment
    // the index multiple times in case of hash collisions.
    uint64_t PrevHash = std::numeric_limits<uint64_t>::max();
    for (auto *HD : Buckets[i]) {
      uint32_t HashValue = HD->HashValue;
      if (PrevHash != HashValue)
        ++index;
      PrevHash = HashValue;
    }
  }
}

void AppleAccelTableBase::emitHashes(AsmPrinter *Asm) {
  uint64_t PrevHash = std::numeric_limits<uint64_t>::max();
  unsigned BucketIdx = 0;
  for (auto &Bucket : Buckets) {
    for (auto &Hash : Bucket) {
      uint32_t HashValue = Hash->HashValue;
      if (PrevHash == HashValue)
        continue;
      Asm->OutStreamer->AddComment("Hash in Bucket " + Twine(BucketIdx));
      Asm->EmitInt32(HashValue);
      PrevHash = HashValue;
    }
    BucketIdx++;
  }
}

void AppleAccelTableBase::emitOffsets(AsmPrinter *Asm,
                                      const MCSymbol *SecBegin) {
  uint64_t PrevHash = std::numeric_limits<uint64_t>::max();
  for (size_t i = 0, e = Buckets.size(); i < e; ++i) {
    for (auto HI = Buckets[i].begin(), HE = Buckets[i].end(); HI != HE; ++HI) {
      uint32_t HashValue = (*HI)->HashValue;
      if (PrevHash == HashValue)
        continue;
      PrevHash = HashValue;
      Asm->OutStreamer->AddComment("Offset in Bucket " + Twine(i));
      MCContext &Context = Asm->OutStreamer->getContext();
      const MCExpr *Sub = MCBinaryExpr::createSub(
          MCSymbolRefExpr::create((*HI)->Sym, Context),
          MCSymbolRefExpr::create(SecBegin, Context), Context);
      Asm->OutStreamer->EmitValue(Sub, sizeof(uint32_t));
    }
  }
}

void AppleAccelTableBase::emitData(AsmPrinter *Asm) {
  for (size_t i = 0, e = Buckets.size(); i < e; ++i) {
    uint64_t PrevHash = std::numeric_limits<uint64_t>::max();
    for (auto &Hash : Buckets[i]) {
      // Terminate the previous entry if there is no hash collision with the
      // current one.
      if (PrevHash != std::numeric_limits<uint64_t>::max() &&
          PrevHash != Hash->HashValue)
        Asm->EmitInt32(0);
      // Remember to emit the label for our offset.
      Asm->OutStreamer->EmitLabel(Hash->Sym);
      Asm->OutStreamer->AddComment(Hash->Name.getString());
      Asm->emitDwarfStringOffset(Hash->Name);
      Asm->OutStreamer->AddComment("Num DIEs");
      Asm->EmitInt32(Hash->Values.size());
      for (const auto *V : Hash->Values) {
        V->emit(Asm);
      }
      PrevHash = Hash->HashValue;
    }
    // Emit the final end marker for the bucket.
    if (!Buckets[i].empty())
      Asm->EmitInt32(0);
  }
}

void AppleAccelTableBase::computeBucketCount() {
  // First get the number of unique hashes.
  std::vector<uint32_t> uniques;
  uniques.reserve(Entries.size());
  for (const auto &E : Entries)
    uniques.push_back(E.second.HashValue);
  array_pod_sort(uniques.begin(), uniques.end());
  std::vector<uint32_t>::iterator p =
      std::unique(uniques.begin(), uniques.end());

  // Compute the hashes count and use it to set that together with the bucket
  // count in the header.
  Header.setBucketAndHashCount(std::distance(uniques.begin(), p));
}

void AppleAccelTableBase::finalizeTable(AsmPrinter *Asm, StringRef Prefix) {
  // Create the individual hash data outputs.
  for (auto &E : Entries) {
    // Unique the entries.
    std::stable_sort(E.second.Values.begin(), E.second.Values.end(),
                     [](const AppleAccelTableData *A,
                        const AppleAccelTableData *B) { return *A < *B; });
    E.second.Values.erase(
        std::unique(E.second.Values.begin(), E.second.Values.end()),
        E.second.Values.end());
  }

  // Figure out how many buckets we need, then compute the bucket contents and
  // the final ordering. We'll emit the hashes and offsets by doing a walk
  // during the emission phase. We add temporary symbols to the data so that we
  // can reference them during the offset later, we'll emit them when we emit
  // the data.
  computeBucketCount();

  // Compute bucket contents and final ordering.
  Buckets.resize(Header.getBucketCount());
  for (auto &E : Entries) {
    uint32_t bucket = E.second.HashValue % Header.getBucketCount();
    Buckets[bucket].push_back(&E.second);
    E.second.Sym = Asm->createTempSymbol(Prefix);
  }

  // Sort the contents of the buckets by hash value so that hash collisions end
  // up together. Stable sort makes testing easier and doesn't cost much more.
  for (auto &Bucket : Buckets)
    std::stable_sort(Bucket.begin(), Bucket.end(),
                     [](HashData *LHS, HashData *RHS) {
                       return LHS->HashValue < RHS->HashValue;
                     });
}

void AppleAccelTableOffsetData::emit(AsmPrinter *Asm) const {
  Asm->EmitInt32(Die->getDebugSectionOffset());
}

void AppleAccelTableTypeData::emit(AsmPrinter *Asm) const {
  Asm->EmitInt32(Die->getDebugSectionOffset());
  Asm->EmitInt16(Die->getTag());
  Asm->EmitInt8(0);
}

void AppleAccelTableStaticOffsetData::emit(AsmPrinter *Asm) const {
  Asm->EmitInt32(Offset);
}

void AppleAccelTableStaticTypeData::emit(AsmPrinter *Asm) const {
  Asm->EmitInt32(Offset);
  Asm->EmitInt16(Tag);
  Asm->EmitInt8(ObjCClassIsImplementation ? dwarf::DW_FLAG_type_implementation
                                          : 0);
  Asm->EmitInt32(QualifiedNameHash);
}

#ifndef _MSC_VER
// The lines below are rejected by older versions (TBD) of MSVC.
constexpr AppleAccelTableHeader::Atom AppleAccelTableTypeData::Atoms[];
constexpr AppleAccelTableHeader::Atom AppleAccelTableOffsetData::Atoms[];
constexpr AppleAccelTableHeader::Atom AppleAccelTableStaticOffsetData::Atoms[];
constexpr AppleAccelTableHeader::Atom AppleAccelTableStaticTypeData::Atoms[];
#else
// FIXME: Erase this path once the minimum MSCV version has been bumped.
const SmallVector<AppleAccelTableHeader::Atom, 4>
    AppleAccelTableOffsetData::Atoms = {AppleAccelTableHeader::Atom(
        dwarf::DW_ATOM_die_offset, dwarf::DW_FORM_data4)};
const SmallVector<AppleAccelTableHeader::Atom, 4>
    AppleAccelTableTypeData::Atoms = {
        AppleAccelTableHeader::Atom(dwarf::DW_ATOM_die_offset,
                                    dwarf::DW_FORM_data4),
        AppleAccelTableHeader::Atom(dwarf::DW_ATOM_die_tag,
                                    dwarf::DW_FORM_data2),
        AppleAccelTableHeader::Atom(dwarf::DW_ATOM_type_flags,
                                    dwarf::DW_FORM_data1)};
const SmallVector<AppleAccelTableHeader::Atom, 4>
    AppleAccelTableStaticOffsetData::Atoms = {AppleAccelTableHeader::Atom(
        dwarf::DW_ATOM_die_offset, dwarf::DW_FORM_data4)};
const SmallVector<AppleAccelTableHeader::Atom, 4>
    AppleAccelTableStaticTypeData::Atoms = {
        AppleAccelTableHeader::Atom(dwarf::DW_ATOM_die_offset,
                                    dwarf::DW_FORM_data4),
        AppleAccelTableHeader::Atom(dwarf::DW_ATOM_die_tag,
                                    dwarf::DW_FORM_data2),
        AppleAccelTableHeader::Atom(5, dwarf::DW_FORM_data1),
        AppleAccelTableHeader::Atom(6, dwarf::DW_FORM_data4)};
#endif

#ifndef NDEBUG
void AppleAccelTableHeader::Header::print(raw_ostream &OS) const {
  OS << "Magic: " << format("0x%x", Magic) << "\n"
     << "Version: " << Version << "\n"
     << "Hash Function: " << HashFunction << "\n"
     << "Bucket Count: " << BucketCount << "\n"
     << "Header Data Length: " << HeaderDataLength << "\n";
}

void AppleAccelTableHeader::Atom::print(raw_ostream &OS) const {
  OS << "Type: " << dwarf::AtomTypeString(Type) << "\n"
     << "Form: " << dwarf::FormEncodingString(Form) << "\n";
}

void AppleAccelTableHeader::HeaderData::print(raw_ostream &OS) const {
  OS << "DIE Offset Base: " << DieOffsetBase << "\n";
  for (auto Atom : Atoms)
    Atom.print(OS);
}

void AppleAccelTableHeader::print(raw_ostream &OS) const {
  Header.print(OS);
  HeaderData.print(OS);
}

void AppleAccelTableBase::HashData::print(raw_ostream &OS) const {
  OS << "Name: " << Name.getString() << "\n";
  OS << "  Hash Value: " << format("0x%x", HashValue) << "\n";
  OS << "  Symbol: ";
  if (Sym)
    OS << *Sym;
  else
    OS << "<none>";
  OS << "\n";
  for (auto *Value : Values)
    Value->print(OS);
}

void AppleAccelTableBase::print(raw_ostream &OS) const {
  // Print Header.
  Header.print(OS);

  // Print Content.
  OS << "Entries: \n";
  for (const auto &Entry : Entries) {
    OS << "Name: " << Entry.first() << "\n";
    for (auto *V : Entry.second.Values)
      V->print(OS);
  }

  OS << "Buckets and Hashes: \n";
  for (auto &Bucket : Buckets)
    for (auto &Hash : Bucket)
      Hash->print(OS);

  OS << "Data: \n";
  for (auto &E : Entries)
    E.second.print(OS);
}

void AppleAccelTableOffsetData::print(raw_ostream &OS) const {
  OS << "  Offset: " << Die->getOffset() << "\n";
}

void AppleAccelTableTypeData::print(raw_ostream &OS) const {
  OS << "  Offset: " << Die->getOffset() << "\n";
  OS << "  Tag: " << dwarf::TagString(Die->getTag()) << "\n";
}

void AppleAccelTableStaticOffsetData::print(raw_ostream &OS) const {
  OS << "  Static Offset: " << Offset << "\n";
}

void AppleAccelTableStaticTypeData::print(raw_ostream &OS) const {
  OS << "  Static Offset: " << Offset << "\n";
  OS << "  QualifiedNameHash: " << format("%x\n", QualifiedNameHash) << "\n";
  OS << "  Tag: " << dwarf::TagString(Tag) << "\n";
  OS << "  ObjCClassIsImplementation: "
     << (ObjCClassIsImplementation ? "true" : "false");
  OS << "\n";
}
#endif
