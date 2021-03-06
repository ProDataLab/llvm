//===----------- CoreAPIsTest.cpp - Unit tests for Core ORC APIs ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "OrcTestCommon.h"
#include "llvm/ExecutionEngine/Orc/Legacy.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::orc;

class SimpleORCResolver : public SymbolResolver {
public:
  using LookupFlagsFn = std::function<SymbolNameSet(SymbolFlagsMap &SymbolFlags,
                                                    const SymbolNameSet &)>;
  using LookupFn = std::function<SymbolNameSet(AsynchronousSymbolQuery &Q,
                                               SymbolNameSet Symbols)>;

  SimpleORCResolver(LookupFlagsFn LookupFlags, LookupFn Lookup)
      : LookupFlags(std::move(LookupFlags)), Lookup(std::move(Lookup)) {}

  SymbolNameSet lookupFlags(SymbolFlagsMap &SymbolFlags,
                            const SymbolNameSet &Symbols) override {
    return LookupFlags(SymbolFlags, Symbols);
  }

  SymbolNameSet lookup(AsynchronousSymbolQuery &Query,
                       SymbolNameSet Symbols) override {
    return Lookup(Query, std::move(Symbols));
  };

private:
  LookupFlagsFn LookupFlags;
  LookupFn Lookup;
};

namespace {

TEST(LegacyAPIInteropTest, QueryAgainstVSO) {

  SymbolStringPool SP;
  ExecutionSession ES(SP);
  auto Foo = SP.intern("foo");

  VSO V;
  SymbolMap Defs;
  JITEvaluatedSymbol FooSym(0xdeadbeef, JITSymbolFlags::Exported);
  Defs[Foo] = FooSym;
  cantFail(V.define(std::move(Defs)));

  auto LookupFlags = [&](SymbolFlagsMap &SymbolFlags,
                         const SymbolNameSet &Names) {
    return V.lookupFlags(SymbolFlags, Names);
  };

  auto Lookup = [&](AsynchronousSymbolQuery &Query, SymbolNameSet Symbols) {
    auto R = V.lookup(Query, Symbols);
    EXPECT_TRUE(R.MaterializationWork.empty())
        << "Query resulted in unexpected materialization work";
    return std::move(R.UnresolvedSymbols);
  };

  SimpleORCResolver UnderlyingResolver(std::move(LookupFlags),
                                       std::move(Lookup));
  JITSymbolResolverAdapter Resolver(ES, UnderlyingResolver);

  JITSymbolResolver::LookupSet Names{StringRef("foo")};

  auto LFR = Resolver.lookupFlags(Names);
  EXPECT_TRUE(!!LFR) << "lookupFlags failed";
  EXPECT_EQ(LFR->size(), 1U)
      << "lookupFlags returned the wrong number of results";
  EXPECT_EQ(LFR->count(*Foo), 1U)
      << "lookupFlags did not contain a result for 'foo'";
  EXPECT_EQ((*LFR)[*Foo], FooSym.getFlags())
      << "lookupFlags contained the wrong result for 'foo'";

  auto LR = Resolver.lookup(Names);
  EXPECT_TRUE(!!LR) << "lookup failed";
  EXPECT_EQ(LR->size(), 1U) << "lookup returned the wrong number of results";
  EXPECT_EQ(LR->count(*Foo), 1U) << "lookup did not contain a result for 'foo'";
  EXPECT_EQ((*LR)[*Foo].getFlags(), FooSym.getFlags())
      << "lookup returned the wrong result for flags of 'foo'";
  EXPECT_EQ((*LR)[*Foo].getAddress(), FooSym.getAddress())
      << "lookup returned the wrong result for address of 'foo'";
}

TEST(LegacyAPIInteropTset, LegacyLookupHelpersFn) {
  constexpr JITTargetAddress FooAddr = 0xdeadbeef;
  JITSymbolFlags FooFlags = JITSymbolFlags::Exported;

  bool BarMaterialized = false;
  constexpr JITTargetAddress BarAddr = 0xcafef00d;
  JITSymbolFlags BarFlags = static_cast<JITSymbolFlags::FlagNames>(
      JITSymbolFlags::Exported | JITSymbolFlags::Weak);

  auto LegacyLookup = [&](const std::string &Name) -> JITSymbol {
    if (Name == "foo")
      return {FooAddr, FooFlags};

    if (Name == "bar") {
      auto BarMaterializer = [&]() -> Expected<JITTargetAddress> {
        BarMaterialized = true;
        return BarAddr;
      };

      return {BarMaterializer, BarFlags};
    }

    return nullptr;
  };

  SymbolStringPool SP;
  auto Foo = SP.intern("foo");
  auto Bar = SP.intern("bar");
  auto Baz = SP.intern("baz");

  SymbolNameSet Symbols({Foo, Bar, Baz});

  SymbolFlagsMap SymbolFlags;
  auto SymbolsNotFound =
      lookupFlagsWithLegacyFn(SymbolFlags, Symbols, LegacyLookup);

  EXPECT_TRUE(!!SymbolsNotFound) << "lookupFlagsWithLegacy failed unexpectedly";
  EXPECT_EQ(SymbolFlags.size(), 2U) << "Wrong number of flags returned";
  EXPECT_EQ(SymbolFlags.count(Foo), 1U) << "Flags for foo missing";
  EXPECT_EQ(SymbolFlags.count(Bar), 1U) << "Flags for foo missing";
  EXPECT_EQ(SymbolFlags[Foo], FooFlags) << "Wrong flags for foo";
  EXPECT_EQ(SymbolFlags[Bar], BarFlags) << "Wrong flags for foo";
  EXPECT_EQ(SymbolsNotFound->size(), 1U) << "Expected one symbol not found";
  EXPECT_EQ(SymbolsNotFound->count(Baz), 1U)
      << "Expected symbol baz to be not found";
  EXPECT_FALSE(BarMaterialized)
      << "lookupFlags should not have materialized bar";

  bool OnResolvedRun = false;
  bool OnReadyRun = false;
  auto OnResolved = [&](Expected<SymbolMap> Result) {
    OnResolvedRun = true;
    EXPECT_TRUE(!!Result) << "lookuWithLegacy failed to resolve";
    EXPECT_EQ(Result->size(), 2U) << "Wrong number of symbols resolved";
    EXPECT_EQ(Result->count(Foo), 1U) << "Result for foo missing";
    EXPECT_EQ(Result->count(Bar), 1U) << "Result for bar missing";
    EXPECT_EQ((*Result)[Foo].getAddress(), FooAddr) << "Wrong address for foo";
    EXPECT_EQ((*Result)[Foo].getFlags(), FooFlags) << "Wrong flags for foo";
    EXPECT_EQ((*Result)[Bar].getAddress(), BarAddr) << "Wrong address for bar";
    EXPECT_EQ((*Result)[Bar].getFlags(), BarFlags) << "Wrong flags for bar";
  };
  auto OnReady = [&](Error Err) {
    EXPECT_FALSE(!!Err) << "Finalization unexpectedly failed";
    OnReadyRun = true;
  };

  AsynchronousSymbolQuery Q({Foo, Bar}, OnResolved, OnReady);
  auto Unresolved = lookupWithLegacyFn(Q, Symbols, LegacyLookup);

  EXPECT_TRUE(OnResolvedRun) << "OnResolved was not run";
  EXPECT_TRUE(OnReadyRun) << "OnReady was not run";
  EXPECT_EQ(Unresolved.size(), 1U) << "Expected one unresolved symbol";
  EXPECT_EQ(Unresolved.count(Baz), 1U) << "Expected baz to be unresolved";
}

} // namespace
