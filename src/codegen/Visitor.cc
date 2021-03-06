// Copyright (c) 2012, Rasmus Andersson. All rights reserved. Use of this source
// code is governed by a MIT-style license that can be found in the LICENSE file.
#include "_VisitorImplHeader.h"


void Visitor::dumpBlockSymbols() {
  if (blockStack_.empty()) return;
  const char *indent = "                                                                        ";
  BlockStack::const_iterator it = blockStack_.begin();
  int L = 1;
  std::cerr << "{" << std::endl;
  
  for (; it != blockStack_.end(); ++it, ++L) {
    BlockScope *bs = *it;
    BasicBlock *bb = bs->block();
    std::string ind(indent, std::min<int>(L*2, strlen(indent)));
    llvm::ValueSymbolTable* symbols = bb->getValueSymbolTable();
    llvm::ValueSymbolTable::const_iterator it2 = symbols->begin();
    
    for (; it2 != symbols->end(); ++it2) {
      std::cerr << ind << it2->getKey().str() << ": ";
    
      llvm::Value *V = it2->getValue();
      llvm::Type *T = V->getType();
      if (T->isLabelTy()) {
        std::cerr << "label" << std::endl;
      } else {
        if (T->isPointerTy()) std::cerr << "pointer ";
        V->dump();
      }
    }
    
    if (L-1 != blockStack_.size()-1) {
      std::cerr << ind << "{" << std::endl;
    }
  }
  
  while (--L) {
    std::string ind(indent, std::min<int>((L-1)*2, strlen(indent)));
    std::cerr << ind << "}" << std::endl;
  }
}


// Returns a module-global uniqe mangled name rooted in *name*
std::string Visitor::uniqueMangledName(const Text& name) const {
  std::string mname;
  std::string utf8name = name.UTF8String();
  int i = 0;
  while (1) {
    // Take the LHS var name and use it for the function name
    if (i > 0) {
      char buf[12]; snprintf(buf, 12, "__%d", i);
      mname = mangledName(utf8name + buf);
    } else {
      mname = mangledName(utf8name);
    }
    if (module_->getNamedValue(mname) == 0 && module_->getNamedGlobal(mname) == 0) {
      // We got a unique name
      break;
    }
    ++i;
  }
  return mname;
}


const char* Visitor::typeName(const Type* T) const {
  if (T == 0) return "<null>";
  // Type::TypeID from Type.h:
  switch(T->getTypeID()) {
    case Type::VoidTyID: return "void";    ///<  0: type with no size
    case Type::FloatTyID: return "float";       ///<  1: 32-bit floating point type
    case Type::DoubleTyID: return "double";      ///<  2: 64-bit floating point type
    case Type::X86_FP80TyID: return "fp80";    ///<  3: 80-bit floating point type (X87)
    case Type::FP128TyID: return "fp128-m112";       ///<  4: 128-bit floating point type (112-bit mantissa)
    case Type::PPC_FP128TyID: return "fp64x2";   ///<  5: 128-bit floating point type (two 64-bits, PowerPC)
    case Type::LabelTyID: return "label";       ///<  6: Labels
    case Type::MetadataTyID: return "metadata";    ///<  7: Metadata
    case Type::X86_MMXTyID: return "mmxvec";     ///<  8: MMX vectors (64 bits, X86 specific)

    // Derived types... see DerivedTypes.h file.
    case Type::IntegerTyID: return "integer";     ///<  9: Arbitrary bit width integers
    case Type::FunctionTyID: return "function";    ///< 10: Functions
    case Type::StructTyID: return "struct";      ///< 11: Structures
    case Type::ArrayTyID: return "array";       ///< 12: Arrays
    case Type::PointerTyID: return "pointer";     ///< 13: Pointers
    case Type::VectorTyID: return "vector";      ///< 14: SIMD 'packed' format, or other vector type
    default: return "?";
  }
}


GlobalVariable* Visitor::createPrivateConstantGlobal(Constant* constantV, const Twine &name) {
  GlobalVariable *gV = new GlobalVariable(
    *module_, // Module M
    constantV->getType(), // Type* Ty
    true, // bool isConstant
    GlobalValue::PrivateLinkage, // LinkageTypes Linkage
    constantV, // Constant *Initializer
    name // const Twine &Name
    // GlobalVariable *InsertBefore = 0
    // bool ThreadLocal = false
    // unsigned AddressSpace = 0
    );

  gV->setName(name);
  gV->setUnnamedAddr(true);
  gV->setAlignment(1);

  return gV;
}


GlobalVariable* Visitor::createStruct(Constant** constants, size_t count, const Twine &name) {
  // Get or create an anonymous struct for constants
  Constant* arrayStV = ConstantStruct::getAnon(makeArrayRef(constants, count), true);
  //rlog("arrayStV: "); arrayStV->dump();
  
  // Put the struct into global variable so we can pass it around
  GlobalVariable* gArrayStV = createPrivateConstantGlobal(arrayStV, name);
  //rlog("gArrayStV: "); gArrayStV->dump();

  return gArrayStV;
}


GlobalVariable* Visitor::createArray(Constant* constantArray, const Twine &name) {
  assert(ConstantAggregateZero::classof(constantArray) || ConstantArray::classof(constantArray));
  
  // Create our struct: <{ i64 N, [i8 x N] }>
  Constant* stV[2];
  uint64_t length = static_cast<ArrayType*>(constantArray->getType())->getNumElements();
  stV[0] = ConstantInt::get(getGlobalContext(), APInt(64, length, false));
  stV[1] = constantArray;
  //stV[1] = createPrivateConstantGlobal(constantArray, "gv");
  
  return createStruct(stV, 2, name);
}


// Returns true if V can be used as the target in a call instruction
// static
FunctionType* Visitor::functionTypeForValue(Value* V) {
  if (V == 0) return 0;
  
  Type* T = V->getType();
  
  if (T->isFunctionTy()) {
    return static_cast<FunctionType*>(T);
  } else if (   T->isPointerTy()
             && T->getNumContainedTypes() == 1
             && T->getContainedType(0)->isFunctionTy() ) {
    return static_cast<FunctionType*>(T->getContainedType(0));
  } else {
    return 0;
  }
}

llvm::Type* Visitor::returnTypeForFunctionType(const ast::FunctionType* astFT) {
  // Figure out return type (unless it's been overridden by returnType) if
  // the interface declares the return type.
  if (astFT->resultTypeIsUnknown()) {
    return builder_.getVoidTy();
  } else {
    return IRTypeForASTType(astFT->resultType());
  }
}


bool Visitor::BlockScope::setFunctionSymbolTarget(const Text& name, ast::FunctionType* hueT,
                                                  FunctionType* FT, Value *V) {
  //rlog("setFunctionSymbolTarget: name: " << name);
  
  FunctionSymbolTargetList& funcs = functions_[name];
  
  if (funcs.size() != 0) {
    // Check existing function types, making sure there's no implementation for FT
    FunctionSymbolTargetList::const_iterator it = funcs.begin();
    for (; it != funcs.end(); ++it) {
      const FunctionSymbolTarget& exstingFS = (*it);
      // Compare function types
      if (exstingFS.type == FT) {
        rlog("Duplicate functions");
        return false;
      }
    }
    
  }
  
  FunctionSymbolTarget symbol;
  symbol.hueType = hueT;
  symbol.type = FT;
  symbol.value = V;
  symbol.owningScope = this;
  funcs.push_back(symbol);
  
  return true;
}


Visitor::FunctionSymbolTargetList Visitor::lookupFunctionSymbols(const ast::Symbol& symbol) const {
  FunctionSymbolTargetList found;

  // FIXME: This needs to resolve actual symbols
  const Text& name = symbol.pathname().size() > 0 ? symbol.pathname()[0] : Text::Empty;
  
  // Scan symbol maps starting at top of stack moving down
  BlockStack::const_reverse_iterator bsit = blockStack_.rbegin();
  for (; bsit != blockStack_.rend(); ++bsit) {
    BlockScope* bs = *bsit;
    
    const FunctionSymbolTargetList* funcSymbols = bs->lookupFunctionSymbolTargets(name);
    if (funcSymbols != 0) {
      // insert ( iterator position, InputIterator first, InputIterator last
      found.insert(found.end(), funcSymbols->begin(), funcSymbols->end());
    }
  }
  
  return found;
}


// ----------- trivial generators ------------


llvm::Module *Visitor::genModule(llvm::LLVMContext& context, const Text moduleName, ast::Function *root) {
  DEBUG_TRACE_LLVM_VISITOR;
  llvm::Module *module = new Module(moduleName.UTF8String(), context);
  
  module_ = module;
  llvm::Value *returnValue = llvm::ConstantInt::get(llvm::getGlobalContext(), APInt(64, 0, true));
  //llvm::Value *moduleFunc = codegenFunction(root, "main", std::string("minit__") + moduleName, builder_.getInt64Ty());
  llvm::Value *moduleFunc = codegenFunction(root, "main", "main", returnValue->getType(), returnValue);
  module_ = 0;
  
  // Failure?
  if (moduleFunc == 0) {
    delete module;
    module = 0;
  }
  
  return module;
}

// ExternalFunction
Value *Visitor::codegenExternalFunction(const ast::ExternalFunction* node) {
  DEBUG_TRACE_LLVM_VISITOR;
  
  // Figure out return type (unless it's been overridden by returnType) if
  // the interface declares the return type.
  Type* returnType = IRTypeForASTType(node->functionType()->resultType());
  if (returnType == 0) return error("Unable to transcode return type from AST to IR");
  
  return codegenFunctionType(node->functionType(), node->name().UTF8String(), returnType);
}

// Block
Value *Visitor::codegenBlock(const ast::Block *block) {
  DEBUG_TRACE_LLVM_VISITOR;
  
  const ast::ExpressionList& expressions = block->expressions();
  ast::ExpressionList::const_iterator it1 = expressions.begin();
  Value *lastValue = 0;
  for (; it1 < expressions.end(); it1++) {
    lastValue = codegen(*it1);
    if (lastValue == 0) return 0;
  }


  //rlog("codegen'd block: "); BB->dump();
  
  //rlog("block's last value: "); lastValue->dump();
  //if (lastValue->getType()->isFunctionTy()) {
  //  rlog("ZOMG FUNC");
  //}
  
  return (lastValue == 0) ? error("Empty block") : lastValue;
}

// Int
Value *Visitor::codegenIntLiteral(const ast::IntLiteral *literal, bool fixedSize) {
  DEBUG_TRACE_LLVM_VISITOR;
  // TODO: Infer the minimal size needed if fixedSize is false
  const unsigned numBits = 64;
  return ConstantInt::get(getGlobalContext(), APInt(numBits, literal->text().UTF8String(), literal->radix()));
}

// Float
Value *Visitor::codegenFloatLiteral(const ast::FloatLiteral *literal, bool fixedSize) {
  DEBUG_TRACE_LLVM_VISITOR;
  // TODO: Infer the minimal size needed if fixedSize is false
  const fltSemantics& size = APFloat::IEEEdouble;
  return ConstantFP::get(getGlobalContext(), APFloat(size, literal->text().UTF8String()));
}

Value *Visitor::codegenBoolLiteral(const ast::BoolLiteral *literal) {
  DEBUG_TRACE_LLVM_VISITOR;
  return literal->isTrue() ? ConstantInt::getTrue(getGlobalContext())
                           : ConstantInt::getFalse(getGlobalContext());
}


Visitor::SymbolTarget Visitor::SymbolTarget::Empty;

#include "_VisitorImplFooter.h"
