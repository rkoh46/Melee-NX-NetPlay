// GuestMemLower: rewrites a module compiled for the GameCube (clang -target powerpc-unknown-eabi,
// 32-bit big-endian pointers) so it runs on the console against guest memory. See docs/NATIVE_C.md.
//
// Every function is re-emitted with each pointer demoted to an i32 guest address:
//   GEP            -> i32 arithmetic with the PowerPC layout's offsets
//   load/store     -> gml_ld*/gml_st* (guest RAM, byte-swapped; MMIO, locked cache and the pipe
//                     on their slow path)
//   alloca         -> a frame on the guest stack (r1), so addresses of locals are guest addresses
//   global, &func  -> the guest address from the symbol map (the game's own data and code)
//   call (defined) -> direct native call
//   call (other)   -> gml_guest_call through the PowerPC EABI (r3..r10, f1..f8) into the translation
// Built as an opt plugin: opt -load-pass-plugin=GuestMemLower.so -passes=gml-lower -gml-symbols=F
// SPDX-License-Identifier: GPL-3.0-or-later
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

using namespace llvm;

static cl::opt<std::string> SymbolsPath("gml-symbols", cl::desc("name address lines (guest symbol map)"), cl::init(""));
static cl::opt<std::string> TargetDL("gml-host-datalayout", cl::desc("host data layout"),
                                     cl::init("e-m:e-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"));
static cl::opt<std::string> TargetTriple("gml-host-triple", cl::init("aarch64-unknown-none-elf"));
static cl::opt<std::string> DolPath("gml-dol", cl::desc("main.dol: constant data without a symbol is found by content"), cl::init(""));
static cl::opt<std::string> SplitsPath("gml-splits", cl::desc("the decomp's splits.txt (per-file address ranges)"), cl::init(""));
static cl::opt<std::string> TranslationUnit("gml-tu", cl::desc("this module's source file as splits.txt names it"), cl::init(""));
static cl::opt<std::string> NativeList("gml-native", cl::desc("functions defined natively in any module (direct calls)"), cl::init(""));
static cl::opt<std::string> KeepGuest("gml-keep-guest", cl::desc("functions that stay translated (Slippi patches them)"), cl::init(""));
static cl::opt<std::string> GuestReport("gml-report-guest", cl::desc("append the functions that stay translated to this file"), cl::init(""));

namespace {
[[noreturn]] void fail(const Twine& msg) {
  errs() << "gml-lower: " << msg << "\n";
  exit(1);
}

constexpr unsigned kOutArgWords = 8;
constexpr uint64_t kScratch = 256;     // by-value copies of one call (checked at each call)   // outgoing stack-argument words reserved at 8(r1) of every frame
struct Lowerer {
  Module& M;
  const DataLayout GDL;          // the guest (PowerPC) layout the IR was built for
  LLVMContext& C;
  Type* I32;
  std::multimap<std::string, uint32_t> syms;      // every symbol, by name and by base name (var$123 -> var)
  std::vector<std::pair<uint32_t, uint32_t>> tuRanges;   // this file's sections (splits.txt)
  std::set<std::string> natives;
  std::map<Function*, Function*> fmap;   // old -> new
  std::map<std::string, FunctionCallee> helpers;
  Function* panicFn = nullptr;
  std::set<std::string> unresolved;
  // Read-only file statics with no copy in the game's image (MWCC folded them away) whose address
  // never escapes (only indexed and loaded): kept as host constants, read natively.
  std::map<const GlobalVariable*, GlobalVariable*> hostConst;
  static bool onlyIndexedLoads(Value* v) {
    for (User* u : v->users()) {
      if (isa<LoadInst>(u)) continue;
      if (auto* g = dyn_cast<GetElementPtrInst>(u); g && g->getPointerOperand() == v) { if (!onlyIndexedLoads(u)) return false; continue; }
      return false;
    }
    return true;
  }

  Lowerer(Module& m) : M(m), GDL(m.getDataLayout()), C(m.getContext()) { I32 = Type::getInt32Ty(C); }

  // ---- constant data found in the game's own image ----
  struct Section { uint32_t addr; std::vector<uint8_t> bytes; };
  std::vector<Section> dataSections;
  std::map<const GlobalVariable*, uint32_t> found;
  std::set<const GlobalVariable*> searching;
  void loadDol() {
    if (DolPath.empty()) return;
    std::ifstream in(DolPath, std::ios::binary);
    std::vector<uint8_t> dol((std::istreambuf_iterator<char>(in)), {});
    auto be = [&](size_t o) { return uint32_t(dol[o]) << 24 | uint32_t(dol[o + 1]) << 16 | uint32_t(dol[o + 2]) << 8 | dol[o + 3]; };
    for (int i = 0; i < 18; ++i) {
      uint32_t off = be(i * 4), addr = be(0x48 + i * 4), size = be(0x90 + i * 4);
      if (!size || off + size > dol.size()) continue;
      dataSections.push_back({addr, std::vector<uint8_t>(dol.begin() + off, dol.begin() + off + size)});
    }
  }
  // The initializer as the GameCube lays it out, pointers resolved to guest addresses.
  bool serialize(Constant* c, std::vector<uint8_t>& out) {
    Type* t = c->getType();
    auto put = [&](uint64_t v, unsigned bytes) { for (int i = bytes - 1; i >= 0; --i) out.push_back(uint8_t(v >> (8 * i))); };
    if (isa<ConstantAggregateZero>(c) || isa<UndefValue>(c)) { out.insert(out.end(), GDL.getTypeAllocSize(t), 0); return true; }
    if (auto* ci = dyn_cast<ConstantInt>(c)) { put(ci->getZExtValue(), GDL.getTypeStoreSize(t)); return true; }
    if (auto* cf = dyn_cast<ConstantFP>(c)) { put(cf->getValueAPF().bitcastToAPInt().getZExtValue(), GDL.getTypeStoreSize(t)); return true; }
    if (t->isPointerTy()) {
      uint32_t a;
      if (!resolveAddr(c, a)) return false;
      put(a, 4); return true;
    }
    if (auto* cds = dyn_cast<ConstantDataSequential>(c)) {
      for (unsigned i = 0; i < cds->getNumElements(); ++i) if (!serialize(cds->getElementAsConstant(i), out)) return false;
      return true;
    }
    if (auto* st = dyn_cast<StructType>(t)) {
      const StructLayout* sl = GDL.getStructLayout(st);
      size_t base = out.size();
      for (unsigned i = 0; i < st->getNumElements(); ++i) {
        out.resize(base + sl->getElementOffset(i), 0);
        if (!serialize(c->getAggregateElement(i), out)) return false;
      }
      out.resize(base + sl->getSizeInBytes(), 0);
      return true;
    }
    if (auto* at = dyn_cast<ArrayType>(t)) {
      for (uint64_t i = 0; i < at->getNumElements(); ++i) if (!serialize(c->getAggregateElement(i), out)) return false;
      return true;
    }
    return false;
  }
  bool resolveAddr(Constant* c, uint32_t& out) {
    if (isa<ConstantPointerNull>(c)) { out = 0; return true; }
    if (auto* g = dyn_cast<GlobalValue>(c)) {
      if (symbol(g->getName(), out)) return true;
      if (auto* gv = dyn_cast<GlobalVariable>(g)) return findInDol(gv, out);
      return false;
    }
    if (auto* ce = dyn_cast<ConstantExpr>(c); ce && ce->getOpcode() == Instruction::GetElementPtr) {
      auto* gep = cast<GEPOperator>(ce);
      APInt off(32, 0);
      uint32_t base;
      if (!gep->accumulateConstantOffset(GDL, off) || !resolveAddr(cast<Constant>(gep->getPointerOperand()), base)) return false;
      out = base + uint32_t(off.getZExtValue());
      return true;
    }
    return false;
  }
  bool findInDol(const GlobalVariable* gv, uint32_t& out) {
    if (auto it = found.find(gv); it != found.end()) { out = it->second; return true; }
    if (!gv->hasInitializer() || dataSections.empty() || searching.count(gv)) return false;
    searching.insert(gv);
    std::vector<uint8_t> want;
    bool ok = serialize(const_cast<Constant*>(gv->getInitializer()), want);
    searching.erase(gv);
    if (!ok || want.empty()) return false;
    // Zeroed data says nothing about where it is; only distinctive content is located this way.
    if (std::all_of(want.begin(), want.end(), [](uint8_t b) { return b == 0; })) return false;
    uint32_t align = std::max<uint32_t>(1, gv->getAlign() ? gv->getAlign()->value() : 1);
    std::vector<uint32_t> hits;
    for (auto& sec : dataSections)
      for (size_t o = 0; o + want.size() <= sec.bytes.size(); ++o)
        if (((sec.addr + o) % align) == 0 && std::equal(want.begin(), want.end(), sec.bytes.begin() + o)) hits.push_back(sec.addr + uint32_t(o));
    // Constant data: any identical copy serves. Writable data must be unambiguous.
    // A file-local static the module only reads behaves as constant data: any identical copy serves.
    const bool constant = gv->isConstant() || gv->getMetadata("gml.const") || gv->getMetadata("gml.readonly");
    if (hits.empty() || (!constant && hits.size() != 1)) return false;
    out = hits.front();
    found[gv] = out;
    return true;
  }

  void loadSymbols() {
    std::ifstream in(SymbolsPath);
    if (!in) fail("cannot read symbol map " + SymbolsPath);
    std::string name; std::string addr;
    while (in >> name >> addr) {
      uint32_t a = (uint32_t)std::stoul(addr, nullptr, 16);
      syms.emplace(name, a);
      size_t dollar = name.find('$');
      if (dollar != std::string::npos && dollar > 0) syms.emplace(name.substr(0, dollar), a);   // MWCC's function statics
    }
    if (!SplitsPath.empty() && !TranslationUnit.empty()) {
      std::ifstream sp(SplitsPath);
      std::string line; bool mine = false;
      while (std::getline(sp, line)) {
        if (!line.empty() && line[0] != '\t' && line[0] != ' ') { mine = line == TranslationUnit + ":"; continue; }
        size_t a = line.find("start:0x"), e = line.find("end:0x");
        if (mine && a != std::string::npos && e != std::string::npos)
          tuRanges.push_back({(uint32_t)std::stoul(line.substr(a + 8), nullptr, 16), (uint32_t)std::stoul(line.substr(e + 6), nullptr, 16)});
      }
      if (tuRanges.empty()) fail("no splits.txt ranges for " + TranslationUnit);
    }
    if (!NativeList.empty()) {
      std::ifstream nl(NativeList);
      while (nl >> name) natives.insert(name);
    }
  }
  bool inTu(uint32_t a) { for (auto& r : tuRanges) if (a >= r.first && a < r.second) return true; return false; }
  // A name may belong to several files' statics: the candidate inside this file's ranges wins.
  bool symbol(StringRef name, uint32_t& out) {
    if (name.ends_with(".gml_old")) name = name.drop_back(8);   // a function being re-emitted (run())
    std::vector<uint32_t> cands;
    auto collect = [&](const std::string& n) { auto [lo, hi] = syms.equal_range(n); for (auto it = lo; it != hi; ++it) cands.push_back(it->second); };
    collect(name.str());
    if (cands.empty()) {   // clang: "function.static" or "static.N" after linking
      size_t dot = name.rfind('.');
      if (dot != StringRef::npos) {
        StringRef tail = name.substr(dot + 1);
        if (!tail.empty() && std::all_of(tail.begin(), tail.end(), ::isdigit)) collect(name.substr(0, dot).str());   // "x.1": renamed by llvm-link
        else collect(tail.str());                                                                                   // "function.static"
      }
    }
    std::sort(cands.begin(), cands.end()); cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
    if (cands.size() == 1) { out = cands[0]; return true; }
    std::vector<uint32_t> local;
    for (uint32_t a : cands) if (inTu(a)) local.push_back(a);
    if (getenv("GML_DEBUG")) { errs() << "symbol " << name << ": " << cands.size() << " candidates, " << local.size() << " in this file\n"; }
    if (local.size() == 1) { out = local[0]; return true; }
    return false;
  }

  Type* mapType(Type* t) {
    if (t->isPointerTy()) return I32;
    if (t->isIntegerTy() || t->isFloatingPointTy() || t->isVoidTy()) return t;
    fail("unsupported first-class type");
  }

  FunctionCallee helper(const std::string& name, Type* ret, ArrayRef<Type*> args) {
    auto it = helpers.find(name);
    if (it != helpers.end()) return it->second;
    auto fc = M.getOrInsertFunction(name, FunctionType::get(ret, args, false));
    helpers.emplace(name, fc);
    return fc;
  }
  std::string suffix(Type* t) {
    if (t->isIntegerTy(8) || t->isIntegerTy(1)) return "8";
    if (t->isIntegerTy(16)) return "16";
    if (t->isIntegerTy(32) || t->isPointerTy()) return "32";
    if (t->isIntegerTy(64)) return "64";
    if (t->isFloatTy()) return "f32";
    if (t->isDoubleTy()) return "f64";
    fail("unsupported memory access type");
  }

  // ---- values ----
  struct FnState {
    Function* oldF; Function* newF;
    DenseMap<Value*, Value*> vmap;
    DenseMap<BasicBlock*, BasicBlock*> bmap;
    Value* frameBase = nullptr;  // guest address of this function's frame (allocas at offsets)
    Value* savedSp = nullptr;
    std::map<AllocaInst*, uint64_t> allocaOffset;
    DenseMap<Value*, Value*> host;   // pointers into host constants (hostConst)
    uint64_t byvalOffset = 0;    // scratch for by-value copies, after the allocas
    uint64_t scratchBase = 0;    // where that scratch starts: each call reuses it (copies live only for the call)
  };

  Value* constAddr(Constant* c, IRBuilder<>& b) {
    if (isa<ConstantPointerNull>(c)) return ConstantInt::get(I32, 0);
    if (isa<UndefValue>(c)) return UndefValue::get(I32);
    if (auto* g = dyn_cast<GlobalValue>(c)) {
      uint32_t a;
      if (resolveAddr(g, a)) return ConstantInt::get(I32, a);
      unresolved.insert(g->getName().str());
      return ConstantInt::get(I32, 0);
    }
    if (auto* ce = dyn_cast<ConstantExpr>(c)) {
      if (ce->getOpcode() == Instruction::GetElementPtr) {
        auto* gep = cast<GEPOperator>(ce);
        APInt off(32, 0);
        if (!gep->accumulateConstantOffset(GDL, off)) fail("non-constant constant GEP");
        Value* base = constAddr(cast<Constant>(gep->getPointerOperand()), b);
        return b.CreateAdd(base, ConstantInt::get(I32, off.getZExtValue()));
      }
      if (ce->getOpcode() == Instruction::IntToPtr) return b.CreateZExtOrTrunc(mapConst(cast<Constant>(ce->getOperand(0)), b), I32);
      if (ce->getOpcode() == Instruction::PtrToInt) return b.CreateZExtOrTrunc(constAddr(cast<Constant>(ce->getOperand(0)), b), ce->getType());
    }
    fail("unsupported constant pointer expression");
  }
  Value* mapConst(Constant* c, IRBuilder<>& b) {
    if (c->getType()->isPointerTy()) return constAddr(c, b);
    if (auto* ce = dyn_cast<ConstantExpr>(c)) {
      if (ce->getOpcode() == Instruction::PtrToInt) return b.CreateZExtOrTrunc(constAddr(cast<Constant>(ce->getOperand(0)), b), ce->getType());
      fail("unsupported constant expression");
    }
    return c;
  }
  Value* V(FnState& s, Value* v, IRBuilder<>& b) {
    if (auto it = s.vmap.find(v); it != s.vmap.end()) return it->second;
    if (auto* c = dyn_cast<Constant>(v)) return mapConst(c, b);
    if (isa<MetadataAsValue>(v)) return v;
    errs() << "unmapped value: " << *v << "\n";
    fail("value used before it was translated");
  }

  Value* hostPointer(FnState& s, Value* p) {
    if (auto it = s.host.find(p); it != s.host.end()) return it->second;
    if (auto* g = dyn_cast<GlobalVariable>(p)) if (auto it = hostConst.find(g); it != hostConst.end()) return it->second;
    return nullptr;
  }
  void findHostConstants() {
    for (auto& g : M.globals()) {
      uint32_t a;
      if (g.isDeclaration() || !g.hasInitializer() || !g.getMetadata("gml.readonly") || resolveAddr(&g, a) || !onlyIndexedLoads(&g)) continue;
      auto* h = new GlobalVariable(M, g.getValueType(), true, GlobalValue::InternalLinkage, g.getInitializer(), "gml.host." + g.getName());
      hostConst[&g] = h;
    }
  }

  // ---- loads and stores ----
  Value* load(Type* t, Value* addr, IRBuilder<>& b) {
    std::string sfx = suffix(t);
    Type* ht = t->isPointerTy() ? I32 : (t->isIntegerTy(1) ? Type::getInt8Ty(C) : t);
    Value* v = b.CreateCall(helper("gml_ld" + sfx, ht, {I32}), {addr});
    if (t->isIntegerTy(1)) v = b.CreateTrunc(v, t);
    return v;
  }
  void store(Value* val, Type* t, Value* addr, IRBuilder<>& b) {
    std::string sfx = suffix(t);
    Type* ht = t->isPointerTy() ? I32 : (t->isIntegerTy(1) ? Type::getInt8Ty(C) : t);
    if (t->isIntegerTy(1)) val = b.CreateZExt(val, ht);
    b.CreateCall(helper("gml_st" + sfx, Type::getVoidTy(C), {I32, ht}), {addr, val});
  }

  // ---- guest calls (PowerPC EABI) ----
  Value* guestCall(FnState& s, CallBase& cb, Value* target, IRBuilder<>& b) {
    unsigned gi = 3, fi = 1, stackWords = 0;
    bool anyFloat = false;
    s.byvalOffset = s.scratchBase;   // this call's by-value copies (previous calls' are dead)
    auto setR = helper("gml_set_r", Type::getVoidTy(C), {I32, I32});
    auto setF = helper("gml_set_f", Type::getVoidTy(C), {I32, Type::getDoubleTy(C)});
    for (unsigned i = 0; i < cb.arg_size(); ++i) {
      Value* a = cb.getArgOperand(i);
      Type* t = a->getType();
      Value* m = V(s, a, b);
      if (cb.isByValArgument(i)) {
        Type* bt = cb.getParamByValType(i);
        uint64_t size = GDL.getTypeAllocSize(bt);
        uint64_t off = (s.byvalOffset + 7) & ~7ull; s.byvalOffset = off + size;
        if (s.byvalOffset > s.scratchBase + kScratch) fail("by-value arguments of one call exceed the frame scratch in " + s.oldF->getName());
        Value* dst = b.CreateAdd(s.frameBase, ConstantInt::get(I32, off));
        b.CreateCall(helper("gml_memcpy", Type::getVoidTy(C), {I32, I32, I32}), {dst, m, ConstantInt::get(I32, size)});
        m = dst; t = PointerType::getUnqual(C);
      }
      if (t->isFloatingPointTy()) {
        Value* d = t->isFloatTy() ? b.CreateFPExt(m, Type::getDoubleTy(C)) : m;
        if (fi > 8) {   // EABI: past f8, doubles (8-byte aligned) in the caller's parameter area
          stackWords = (stackWords + 1) & ~1u;
          if (stackWords + 2 > kOutArgWords) fail("too many stack arguments to a guest call");
          b.CreateCall(helper("gml_stf64", Type::getVoidTy(C), {I32, Type::getDoubleTy(C)}),
                       {b.CreateAdd(s.frameBase, ConstantInt::get(I32, 8 + 4 * stackWords)), d});
          stackWords += 2;
        } else {
          b.CreateCall(setF, {ConstantInt::get(I32, fi++), d});
        }
        anyFloat = true;
      } else if (t->isIntegerTy(64)) {
        if (gi % 2 == 0) ++gi;
        if (gi + 1 > 10) fail("i64 argument out of registers");
        b.CreateCall(setR, {ConstantInt::get(I32, gi++), b.CreateTrunc(b.CreateLShr(m, 32), I32)});
        b.CreateCall(setR, {ConstantInt::get(I32, gi++), b.CreateTrunc(m, I32)});
      } else {
        Value* w = m->getType() == I32 ? m
                 : (cb.paramHasAttr(i, Attribute::SExt) ? b.CreateSExt(m, I32) : b.CreateZExt(m, I32));
        if (gi > 10) {   // EABI: past r10, words in the caller's parameter area at 8(r1)
          if (stackWords >= kOutArgWords) fail("too many stack arguments to a guest call");
          b.CreateCall(helper("gml_st32", Type::getVoidTy(C), {I32, I32}),
                       {b.CreateAdd(s.frameBase, ConstantInt::get(I32, 8 + 4 * stackWords++)), w});
        } else {
          b.CreateCall(setR, {ConstantInt::get(I32, gi++), w});
        }
      }
    }
    if (cb.getFunctionType()->isVarArg())
      b.CreateCall(helper("gml_set_cr6", Type::getVoidTy(C), {Type::getInt1Ty(C)}), {ConstantInt::getBool(C, anyFloat)});
    b.CreateCall(helper("gml_guest_call", Type::getVoidTy(C), {I32}), {target});
    Type* rt = cb.getType();
    if (rt->isVoidTy()) return nullptr;
    auto getR = helper("gml_get_r", I32, {I32});
    if (rt->isFloatingPointTy()) {
      Value* d = b.CreateCall(helper("gml_get_f", Type::getDoubleTy(C), {I32}), {ConstantInt::get(I32, 1)});
      return rt->isFloatTy() ? b.CreateFPTrunc(d, rt) : d;
    }
    Value* r3 = b.CreateCall(getR, {ConstantInt::get(I32, 3)});
    if (rt->isPointerTy() || rt->isIntegerTy(32)) return r3;
    if (rt->isIntegerTy(64)) {
      Value* r4 = b.CreateCall(getR, {ConstantInt::get(I32, 4)});
      return b.CreateOr(b.CreateShl(b.CreateZExt(r3, rt), 32), b.CreateZExt(r4, rt));
    }
    return b.CreateTrunc(r3, rt);
  }

  // ---- instructions ----
  void translate(FnState& s, Instruction& I, IRBuilder<>& b) {
    auto def = [&](Value* v) { if (v) { v->setName(I.getName()); s.vmap[&I] = v; } };
    if (auto* ai = dyn_cast<AllocaInst>(&I)) {
      def(b.CreateAdd(s.frameBase, ConstantInt::get(I32, s.allocaOffset.at(ai))));
      return;
    }
    if (auto* li = dyn_cast<LoadInst>(&I)) {
      if (Value* hp = hostPointer(s, li->getPointerOperand())) { def(b.CreateLoad(li->getType(), hp)); return; }
      def(load(li->getType(), V(s, li->getPointerOperand(), b), b));
      return;
    }
    if (auto* si = dyn_cast<StoreInst>(&I)) {
      store(V(s, si->getValueOperand(), b), si->getValueOperand()->getType(), V(s, si->getPointerOperand(), b), b);
      return;
    }
    if (auto* gep = dyn_cast<GetElementPtrInst>(&I)) {
      if (Value* hp = hostPointer(s, gep->getPointerOperand())) {   // into a host constant: a host GEP
        std::vector<Value*> idx;
        for (auto it = gep->idx_begin(); it != gep->idx_end(); ++it) idx.push_back(V(s, *it, b));
        s.host[&I] = b.CreateGEP(gep->getSourceElementType(), hp, idx);
        return;
      }
      Value* addr = V(s, gep->getPointerOperand(), b);
      for (gep_type_iterator gi = gep_type_begin(gep), ge = gep_type_end(gep); gi != ge; ++gi) {
        Value* idx = gi.getOperand();
        if (StructType* st = gi.getStructTypeOrNull()) {
          uint64_t off = GDL.getStructLayout(st)->getElementOffset(cast<ConstantInt>(idx)->getZExtValue());
          if (off) addr = b.CreateAdd(addr, ConstantInt::get(I32, off));
        } else {
          uint64_t size = gi.getSequentialElementStride(GDL).getFixedValue();
          if (auto* ci = dyn_cast<ConstantInt>(idx)) {
            int64_t off = ci->getSExtValue() * (int64_t)size;
            if (off) addr = b.CreateAdd(addr, ConstantInt::get(I32, (uint32_t)off));
          } else {
            Value* x = V(s, idx, b);
            x = b.CreateSExtOrTrunc(x, I32);
            addr = b.CreateAdd(addr, size == 1 ? x : b.CreateMul(x, ConstantInt::get(I32, size)));
          }
        }
      }
      def(addr);
      return;
    }
    if (auto* phi = dyn_cast<PHINode>(&I)) {
      def(b.CreatePHI(mapType(phi->getType()), phi->getNumIncomingValues()));
      return;  // incoming values are filled after every block exists
    }
    if (auto* cb = dyn_cast<CallBase>(&I)) { translateCall(s, *cb, b, def); return; }
    if (auto* ret = dyn_cast<ReturnInst>(&I)) {
      Value* rv = ret->getReturnValue() ? V(s, ret->getReturnValue(), b) : nullptr;
      if (s.savedSp) b.CreateCall(helper("gml_set_r", Type::getVoidTy(C), {I32, I32}), {ConstantInt::get(I32, 1), s.savedSp});
      if (rv) b.CreateRet(rv); else b.CreateRetVoid();
      return;
    }
    if (auto* br = dyn_cast<BranchInst>(&I)) {
      if (br->isConditional()) b.CreateCondBr(V(s, br->getCondition(), b), s.bmap[br->getSuccessor(0)], s.bmap[br->getSuccessor(1)]);
      else b.CreateBr(s.bmap[br->getSuccessor(0)]);
      return;
    }
    if (auto* sw = dyn_cast<SwitchInst>(&I)) {
      auto* ns = b.CreateSwitch(V(s, sw->getCondition(), b), s.bmap[sw->getDefaultDest()], sw->getNumCases());
      for (auto& cs : sw->cases()) ns->addCase(cs.getCaseValue(), s.bmap[cs.getCaseSuccessor()]);
      return;
    }
    if (isa<UnreachableInst>(&I)) { b.CreateUnreachable(); return; }
    if (auto* ci = dyn_cast<CmpInst>(&I)) {
      Value* l = V(s, ci->getOperand(0), b); Value* r = V(s, ci->getOperand(1), b);
      def(ci->isFPPredicate() ? b.CreateFCmp(ci->getPredicate(), l, r) : b.CreateICmp(ci->getPredicate(), l, r));
      return;
    }
    if (auto* sel = dyn_cast<SelectInst>(&I)) {
      def(b.CreateSelect(V(s, sel->getCondition(), b), V(s, sel->getTrueValue(), b), V(s, sel->getFalseValue(), b)));
      return;
    }
    if (auto* cast = dyn_cast<CastInst>(&I)) {
      Value* x = V(s, cast->getOperand(0), b);
      switch (cast->getOpcode()) {
        case Instruction::PtrToInt: def(b.CreateZExtOrTrunc(x, cast->getType())); return;
        case Instruction::IntToPtr: def(b.CreateZExtOrTrunc(x, I32)); return;
        case Instruction::BitCast:
          if (cast->getType()->isPointerTy()) { def(x); return; }
          def(b.CreateBitCast(x, cast->getType())); return;
        default: def(b.CreateCast(cast->getOpcode(), x, cast->getType())); return;
      }
    }
    if (auto* bo = dyn_cast<BinaryOperator>(&I)) {
      Value* r = b.CreateBinOp(bo->getOpcode(), V(s, bo->getOperand(0), b), V(s, bo->getOperand(1), b));
      if (auto* ri = dyn_cast<Instruction>(r)) ri->copyIRFlags(bo);
      def(r);
      return;
    }
    if (auto* un = dyn_cast<UnaryOperator>(&I)) {
      Value* r = b.CreateUnOp(un->getOpcode(), V(s, un->getOperand(0), b));
      if (auto* ri = dyn_cast<Instruction>(r)) ri->copyIRFlags(un);
      def(r);
      return;
    }
    if (auto* fr = dyn_cast<FreezeInst>(&I)) { def(b.CreateFreeze(V(s, fr->getOperand(0), b))); return; }
    errs() << "unsupported instruction: " << I << "\n";
    fail("unsupported instruction in " + s.oldF->getName());
  }

  template <class Def>
  void translateCall(FnState& s, CallBase& cb, IRBuilder<>& b, Def def) {
    Function* callee = cb.getCalledFunction();
    if (callee && callee->isIntrinsic()) {
      switch (callee->getIntrinsicID()) {
        case Intrinsic::lifetime_start: case Intrinsic::lifetime_end: case Intrinsic::dbg_declare:
        case Intrinsic::dbg_value: case Intrinsic::dbg_assign: case Intrinsic::assume:
          return;
        case Intrinsic::memcpy: case Intrinsic::memmove:
          b.CreateCall(helper(callee->getIntrinsicID() == Intrinsic::memcpy ? "gml_memcpy" : "gml_memmove", Type::getVoidTy(C), {I32, I32, I32}),
                       {V(s, cb.getArgOperand(0), b), V(s, cb.getArgOperand(1), b), b.CreateZExtOrTrunc(V(s, cb.getArgOperand(2), b), I32)});
          return;
        case Intrinsic::memset:
          b.CreateCall(helper("gml_memset", Type::getVoidTy(C), {I32, I32, I32}),
                       {V(s, cb.getArgOperand(0), b), b.CreateZExt(V(s, cb.getArgOperand(1), b), I32), b.CreateZExtOrTrunc(V(s, cb.getArgOperand(2), b), I32)});
          return;
        default: {
          for (auto& a : cb.args()) if (a->getType()->isPointerTy()) fail("pointer intrinsic " + callee->getName());
          std::vector<Value*> args;
          for (auto& a : cb.args()) args.push_back(V(s, a, b));
          // Non-pointer intrinsics keep their types: the same declaration serves.
          Value* r = b.CreateCall(callee->getFunctionType(), callee, args);
          if (!cb.getType()->isVoidTy()) def(r);
          return;
        }
      }
    }
    if (callee && (callee->getName() == "__assert" || callee->getName() == "__assert_func" || callee->getName() == "HSD_Panic" ||
                   callee->getName() == "OSPanic")) {
      b.CreateCall(helper("gml_panic", Type::getVoidTy(C), {}), {});
      return;
    }
    if (callee && callee->getName() == "sqrtf") {   // MSL's inline frsqrte sequence, exact (gml_glue.cpp)
      def(b.CreateCall(helper("gml_sqrtf", Type::getFloatTy(C), {Type::getFloatTy(C)}), {V(s, cb.getArgOperand(0), b)}));
      return;
    }
    if (callee && (callee->getName() == "fabsf" || callee->getName() == "fabs")) {   // exact natively
      def(b.CreateUnaryIntrinsic(Intrinsic::fabs, V(s, cb.getArgOperand(0), b)));
      return;
    }
    if (callee && !callee->isDeclaration()) {
      std::vector<Value*> args;
      s.byvalOffset = s.scratchBase;
      for (unsigned i = 0; i < cb.arg_size(); ++i) {
        Value* a = V(s, cb.getArgOperand(i), b);
        if (cb.isByValArgument(i)) {   // the callee owns a copy: made in this frame's scratch
          uint64_t size = GDL.getTypeAllocSize(cb.getParamByValType(i));
          uint64_t off = (s.byvalOffset + 7) & ~7ull; s.byvalOffset = off + size;
        if (s.byvalOffset > s.scratchBase + kScratch) fail("by-value arguments of one call exceed the frame scratch in " + s.oldF->getName());
          Value* dst = b.CreateAdd(s.frameBase, ConstantInt::get(I32, off));
          b.CreateCall(helper("gml_memcpy", Type::getVoidTy(C), {I32, I32, I32}), {dst, a, ConstantInt::get(I32, size)});
          a = dst;
        }
        args.push_back(a);
      }
      Value* r = b.CreateCall(fmap.at(callee), args);
      if (!cb.getType()->isVoidTy()) def(r);
      return;
    }
    if (callee && natives.count(callee->getName().str())) {   // defined natively in another module
      std::vector<Type*> pts;
      for (Type* t : callee->getFunctionType()->params()) pts.push_back(mapType(t));
      FunctionCallee nf = M.getOrInsertFunction("gml_" + callee->getName().str(), FunctionType::get(mapType(callee->getReturnType()), pts, false));
      std::vector<Value*> args;
      s.byvalOffset = s.scratchBase;
      for (unsigned i = 0; i < cb.arg_size(); ++i) {
        Value* a = V(s, cb.getArgOperand(i), b);
        if (cb.isByValArgument(i)) {
          uint64_t size = GDL.getTypeAllocSize(cb.getParamByValType(i));
          uint64_t off = (s.byvalOffset + 7) & ~7ull; s.byvalOffset = off + size;
        if (s.byvalOffset > s.scratchBase + kScratch) fail("by-value arguments of one call exceed the frame scratch in " + s.oldF->getName());
          Value* dst = b.CreateAdd(s.frameBase, ConstantInt::get(I32, off));
          b.CreateCall(helper("gml_memcpy", Type::getVoidTy(C), {I32, I32, I32}), {dst, a, ConstantInt::get(I32, size)});
          a = dst;
        }
        args.push_back(a);
      }
      Value* r = b.CreateCall(nf, args);
      if (!cb.getType()->isVoidTy()) def(r);
      return;
    }
    Value* target;
    if (callee) {
      uint32_t a;
      if (!symbol(callee->getName(), a)) { unresolved.insert(callee->getName().str()); a = 0; }
      target = ConstantInt::get(I32, a);
    } else {
      target = V(s, cb.getCalledOperand(), b);
    }
    def(guestCall(s, cb, target, b));
  }

  void lowerFunction(Function* oldF) {
    Function* newF = fmap.at(oldF);
    FnState s; s.oldF = oldF; s.newF = newF;
    auto ai = newF->arg_begin();
    for (auto& a : oldF->args()) { ai->setName(a.getName()); s.vmap[&a] = &*ai; ++ai; }
    for (auto& bb : *oldF) s.bmap[&bb] = BasicBlock::Create(C, bb.getName(), newF);
    // Frame: every alloca at a fixed offset, by-value scratch after; 8 bytes at the bottom are the
    // PowerPC back chain and the LR slot a guest callee writes.
    uint64_t off = 8 + 4 * kOutArgWords; bool any = false;
    for (auto& I : instructions(oldF))
      if (auto* a = dyn_cast<AllocaInst>(&I)) {
        if (!a->isStaticAlloca()) fail("dynamic alloca in " + oldF->getName());
        uint64_t align = std::max<uint64_t>(a->getAlign().value(), 4);
        off = (off + align - 1) & ~(align - 1);
        s.allocaOffset[a] = off;
        off += *a->getAllocationSize(GDL) ? a->getAllocationSize(GDL)->getFixedValue() : 0;
        any = true;
      }
    bool calls = false;
    for (auto& I : instructions(oldF))
      if (auto* cb = dyn_cast<CallBase>(&I)) {
        Function* f = cb->getCalledFunction();
        if (!f || (f->isDeclaration() && !f->isIntrinsic())) calls = true;
        for (unsigned i = 0; i < cb->arg_size(); ++i) if (cb->isByValArgument(i)) calls = true;
      }
    IRBuilder<> b(s.bmap[&oldF->getEntryBlock()]);
    if (any || calls) {
      uint64_t scratch = kScratch;   // by-value argument copies, reused by every call
      uint64_t frame = (off + scratch + 15) & ~15ull;
      auto getR = helper("gml_get_r", I32, {I32});
      s.savedSp = b.CreateCall(getR, {ConstantInt::get(I32, 1)}, "sp");
      Value* nsp = b.CreateSub(s.savedSp, ConstantInt::get(I32, frame), "frame");
      b.CreateCall(helper("gml_st32", Type::getVoidTy(C), {I32, I32}), {nsp, s.savedSp});   // back chain
      b.CreateCall(helper("gml_set_r", Type::getVoidTy(C), {I32, I32}), {ConstantInt::get(I32, 1), nsp});
      s.frameBase = nsp;
      s.byvalOffset = off;
      s.scratchBase = off;
    }
    ReversePostOrderTraversal<Function*> rpot(oldF);
    for (BasicBlock* bb : rpot) {
      b.SetInsertPoint(s.bmap[bb]);
      for (auto& I : *bb) translate(s, I, b);
    }
    for (BasicBlock* bb : rpot)
      for (auto& I : *bb)
        if (auto* phi = dyn_cast<PHINode>(&I)) {
          auto* np = cast<PHINode>(s.vmap[phi]);
          for (unsigned k = 0; k < phi->getNumIncomingValues(); ++k) {
            BasicBlock* inb = s.bmap[phi->getIncomingBlock(k)];
            IRBuilder<> pb(inb->getTerminator());
            np->addIncoming(V(s, phi->getIncomingValue(k), pb), inb);
          }
        }
    // Blocks unreachable in the original stay empty: give them a terminator.
    for (auto& bb : *newF) if (!bb.getTerminator()) { IRBuilder<> eb(&bb); eb.CreateUnreachable(); }
  }

  void run() {
    loadSymbols();
    loadDol();
    findHostConstants();
    // C library routines a header defines inline (memcpy, memset, ...) stay the game's own: native
    // code calls Melee's copies, so even overlapping or odd uses behave exactly as on the GameCube.
    for (const char* n : {"memcpy", "memmove", "memset", "memcmp", "strlen", "strcpy", "strncpy", "strcmp", "strncmp", "strcat"})
      if (Function* f = M.getFunction(n); f && !f->isDeclaration()) { f->deleteBody(); f->setLinkage(GlobalValue::ExternalLinkage); }
    // A function that refers to something with no guest address (an MWCC builtin, data the image
    // does not hold) stays translated: its body is dropped and calls to it go to the game.
    std::set<Function*> keepGuest;
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto& F : M) {
        if (F.isDeclaration() || keepGuest.count(&F)) continue;
        std::string why;
        for (auto& I : instructions(F)) {
          if (auto* cb = dyn_cast<CallBase>(&I))   // assertion arguments: the call becomes a native panic
            if (Function* cf = cb->getCalledFunction(); cf && (cf->getName() == "__assert" || cf->getName() == "__assert_func" ||
                                                               cf->getName() == "HSD_Panic" || cf->getName() == "OSPanic")) continue;
          for (Value* op : I.operands()) {
            auto* g = dyn_cast<GlobalValue>(op->stripPointerCasts());
            if (!g) continue;
            if (auto* f = dyn_cast<Function>(g)) {
              if (f->isIntrinsic() || !f->isDeclaration() || natives.count(f->getName().str())) continue;
              static const std::set<std::string> special = {"sqrtf", "fabsf", "fabs", "__assert", "__assert_func", "HSD_Panic", "OSPanic"};
              if (special.count(f->getName().str())) continue;
            } else if (auto* gv = dyn_cast<GlobalVariable>(g); gv && hostConst.count(gv)) continue;
            uint32_t a;
            if (!resolveAddr(g, a)) { why = g->getName().str(); break; }
          }
          if (!why.empty()) break;
        }
        if (!why.empty()) {
          errs() << "gml-lower: " << F.getName() << " stays translated (" << why << " has no guest address)\n";
          if (!GuestReport.empty()) { std::ofstream rep(GuestReport, std::ios::app); rep << F.getName().str() << "\n"; }
          keepGuest.insert(&F);
          F.deleteBody();
          F.setLinkage(GlobalValue::ExternalLinkage);
          changed = true;
        }
      }
    }
    std::vector<Function*> defined;
    for (auto& F : M) if (!F.isDeclaration()) defined.push_back(&F);
    for (Function* F : defined) {
      std::vector<Type*> params;
      for (auto& a : F->args()) params.push_back(mapType(a.getType()));
      auto* ft = FunctionType::get(mapType(F->getReturnType()), params, false);
      std::string name = F->getName().str();
      F->setName(name + ".gml_old");
      // gml_<name>: the game's C names may also exist elsewhere in the program (Aurora, host code).
      // A static with a unique symbol in the game stays visible: the translation also reaches it
      // through function pointers (class methods), so it needs a hookable native version, which
      // inlining would otherwise leave unused and the optimiser would drop.
      auto linkage = F->getLinkage();
      if (F->hasLocalLinkage() && syms.count(name) == 1) linkage = GlobalValue::ExternalLinkage;
      Function* nf = Function::Create(ft, linkage, "gml_" + name, &M);
      // Integer extension attributes say how narrow values sit in a 32-bit register (the wrappers
      // read them from the lowered IR to extend a returned u8/s8 the way the PowerPC caller expects).
      for (auto k : {Attribute::ZExt, Attribute::SExt}) {
        if (F->getAttributes().hasRetAttr(k)) nf->addRetAttr(k);
        for (unsigned i = 0; i < F->arg_size(); ++i) if (F->hasParamAttribute(i, k)) nf->addParamAttr(i, k);
      }
      nf->addFnAttr(Attribute::getWithUWTableKind(C, UWTableKind::Default));
      // -fno-jump-tables kept the optimizer from inventing lookup tables in guest data (they have no
      // guest address); the host code may use jump tables again.
      fmap[F] = nf;
    }
    for (Function* F : defined) lowerFunction(F);
    for (Function* F : defined) { F->replaceAllUsesWith(UndefValue::get(F->getType())); F->eraseFromParent(); }
    // Whatever still refers to guest-layout globals is gone with the old bodies.
    std::vector<GlobalVariable*> gvs;
    std::set<GlobalVariable*> keep;
    for (auto& [o, h] : hostConst) keep.insert(h);
    for (auto& g : M.globals()) if (!keep.count(&g)) gvs.push_back(&g);
    for (auto* g : gvs) { g->replaceAllUsesWith(UndefValue::get(g->getType())); g->eraseFromParent(); }
    std::vector<Function*> dead;
    for (auto& F : M) if (F.isDeclaration() && F.use_empty()) dead.push_back(&F);
    for (auto* F : dead) F->eraseFromParent();
    if (!unresolved.empty()) {
      errs() << "gml-lower: no guest address for:";
      for (auto& n : unresolved) errs() << " " << n;
      errs() << "\n";
      fail("unresolved symbols");
    }
    M.setDataLayout(TargetDL);
    M.setTargetTriple(TargetTriple);
  }
};

// Before optimisation: every global is the game's memory, shared with the translated code and with
// Slippi's patches. A `static` the file never reads is still read elsewhere in the game, and a const
// table may be patched at run time, so none may be removed, localised or folded.
struct GuestExternalizePass : PassInfoMixin<GuestExternalizePass> {
  // Whether code in this module may write the global (anything but loads through GEPs counts).
  static bool written(Value* v) {
    for (User* u : v->users()) {
      if (isa<LoadInst>(u)) continue;
      if (isa<GetElementPtrInst>(u) || isa<GEPOperator>(u) || isa<BitCastOperator>(u)) { if (written(u)) return true; continue; }
      return true;
    }
    return false;
  }
  PreservedAnalyses run(Module& M, ModuleAnalysisManager&) {
    // Functions Slippi's Gecko codes patch stay translated: their bodies go before the inliner can
    // copy them into native callers, so every call reaches the patched translation.
    if (!KeepGuest.empty()) {
      std::ifstream in(KeepGuest); std::string n;
      while (std::getline(in, n))
        if (Function* f = M.getFunction(n); f && !f->isDeclaration()) { f->deleteBody(); f->setLinkage(GlobalValue::ExternalLinkage); }
    }
    for (auto& g : M.globals()) {
      if (g.isDeclaration() || g.getName().starts_with("llvm.")) continue;
      const bool local = g.hasLocalLinkage();
      g.setLinkage(GlobalValue::ExternalLinkage);
      g.setVisibility(GlobalValue::DefaultVisibility);
      g.setUnnamedAddr(GlobalValue::UnnamedAddr::None);
      if (g.isConstant()) g.setMetadata("gml.const", MDNode::get(M.getContext(), {}));   // for the DOL search
      if (local && !written(&g)) g.setMetadata("gml.readonly", MDNode::get(M.getContext(), {}));
      g.setConstant(false);
    }
    return PreservedAnalyses::none();
  }
};

struct GuestMemLowerPass : PassInfoMixin<GuestMemLowerPass> {
  PreservedAnalyses run(Module& M, ModuleAnalysisManager&) {
    Lowerer(M).run();
    return PreservedAnalyses::none();
  }
};
}  // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "GuestMemLower", "0.1", [](PassBuilder& pb) {
            pb.registerPipelineParsingCallback([](StringRef name, ModulePassManager& mpm, ArrayRef<PassBuilder::PipelineElement>) {
              if (name == "gml-lower") { mpm.addPass(GuestMemLowerPass()); return true; }
              if (name == "gml-externalize") { mpm.addPass(GuestExternalizePass()); return true; }
              return false;
            });
          }};
}
