//===--- PhiArgumentOptimizations.cpp - phi argument optimizations --------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file contains optimizations for basic block phi arguments.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-optimize-block-arguments"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/Debug.h"

using namespace swift;

namespace {

/// Removes redundant basic block phi-arguments.
///
/// RedundantPhiEliminationPass eliminates block arguments which have
/// the same value as other arguments of the same block. This also works with
/// cycles, like two equivalent loop induction variables. Such patterns are
/// generated e.g. when using stdlib's enumerated() on Array.
///
/// \code
///   preheader:
///     br bb1(%initval, %initval)
///   header(%phi1, %phi2):
///     %next1 = builtin "add" (%phi1, %one)
///     %next2 = builtin "add" (%phi2, %one)
///     cond_br %loopcond, header(%next1, %next2), exit
///   exit:
/// \endcode
///
/// is replaced with
///
/// \code
///   preheader:
///     br bb1(%initval)
///   header(%phi1):
///     %next1 = builtin "add" (%phi1, %one)
///     %next2 = builtin "add" (%phi1, %one) // dead: will be cleaned-up later
///     cond_br %loopcond, header(%next1), exit
///   exit:
/// \endcode
///
/// Any remaining dead or "trivially" equivalent instructions will then be
/// cleaned-up by DCE and CSE, respectively.
///
/// RedundantPhiEliminationPass is not part of SimplifyCFG because
/// * no other SimplifyCFG optimization depends on it.
/// * compile time: it doesn't need to run every time SimplifyCFG runs.
///
class RedundantPhiEliminationPass : public SILFunctionTransform {
public:
  RedundantPhiEliminationPass() {}

  void run() override;

private:
  bool optimizeArgs(SILBasicBlock *block);

  bool valuesAreEqual(SILValue val1, SILValue val2);
};

void RedundantPhiEliminationPass::run() {
  SILFunction *F = getFunction();
  if (!F->shouldOptimize())
    return;

  LLVM_DEBUG(llvm::dbgs() << "*** RedundantPhiElimination on function: "
                          << F->getName() << " ***\n");

  bool changed = false;
  for (SILBasicBlock &block : *getFunction()) {
    changed |= optimizeArgs(&block);
  }

  if (changed) {
    invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
}

#ifndef NDEBUG
static bool hasOnlyNoneOwnershipIncomingValues(SILPhiArgument *phi) {
  SmallSetVector<SILPhiArgument *, 4> worklist;
  SmallVector<SILValue, 4> incomingValues;
  worklist.insert(phi);

  // Size of the worklist changes in this loop
  for (unsigned idx = 0; idx < worklist.size(); idx++) {
    phi->getIncomingPhiValues(incomingValues);
    for (auto incomingValue : incomingValues) {
      if (incomingValue->getOwnershipKind() == OwnershipKind::None)
        continue;
      if (auto *incomingPhi = dyn_cast<SILPhiArgument>(incomingValue)) {
        if (incomingPhi->isPhi()) {
          worklist.insert(incomingPhi);
          continue;
        }
      }
      return false;
    }
    incomingValues.clear();
  }
  return true;
}
#endif

// FIXME: Replace with generic ownership rauw for phi args when it is
// upstreamed.
static void eraseOwnedPhiArgument(SILBasicBlock *block, unsigned argIdx) {
  auto *phi = cast<SILPhiArgument>(block->getArgument(argIdx));
  assert(phi->getOwnershipKind() == OwnershipKind::Owned);

  auto visitor = [&](Operand *op) {
    if (op->isLifetimeEnding()) {
      // Insert a destroy
      SILBuilderWithScope builder(op->getUser());
      builder.createDestroyValue(RegularLocation::getAutoGeneratedLocation(),
                                 op->get());
    }
    return true;
  };
  phi->visitIncomingPhiOperands(visitor);

  erasePhiArgument(block, argIdx);
}

bool RedundantPhiEliminationPass::optimizeArgs(SILBasicBlock *block) {
  // Avoid running into quadratic behavior for blocks which have many arguments.
  // This is seldom, anyway.
  unsigned maxArgumentCombinations = 48;

  bool changed = false;
  unsigned numArgumentCombinations = 0;
  for (unsigned arg1Idx = 0; arg1Idx < block->getNumArguments(); ++arg1Idx) {
    for (unsigned arg2Idx = arg1Idx + 1; arg2Idx < block->getNumArguments();) {
      if (++numArgumentCombinations > maxArgumentCombinations)
        return changed;

      SILArgument *arg1 = block->getArgument(arg1Idx);
      SILArgument *arg2 = block->getArgument(arg2Idx);
      if (!arg1->isPhi() || !arg2->isPhi())
        continue;

      if (valuesAreEqual(arg1, arg2)) {
        if (block->getParent()->hasOwnership()) {
          auto *phi1 = cast<SILPhiArgument>(arg1);
          auto *phi2 = cast<SILPhiArgument>(arg2);
          // @owned phi args can only be equal if all the incoming values had a
          // None ownership. To replace, create a copy_value of the duplicate
          // arg.
          if (phi1->getOwnershipKind() == OwnershipKind::Owned &&
              phi2->getOwnershipKind() == OwnershipKind::Owned) {
            assert(hasOnlyNoneOwnershipIncomingValues(phi1));
            assert(hasOnlyNoneOwnershipIncomingValues(phi2));
            SILBuilderWithScope builder(&block->front());
            auto copy = builder.createCopyValue(
                RegularLocation::getAutoGeneratedLocation(), phi1);
            phi2->replaceAllUsesWith(copy);
            eraseOwnedPhiArgument(block, arg2Idx);
          }
          // If arg2 has none ownership, replace arg1 with arg2
          else if (phi1->getOwnershipKind() == OwnershipKind::Owned &&
                   phi2->getOwnershipKind() == OwnershipKind::None) {
            assert(hasOnlyNoneOwnershipIncomingValues(phi1));
            phi1->replaceAllUsesWith(phi2);
            eraseOwnedPhiArgument(block, arg1Idx);
          }
          // If arg1 has none ownership, replace arg2 with arg1
          else if (phi1->getOwnershipKind() == OwnershipKind::None &&
                   phi2->getOwnershipKind() == OwnershipKind::Owned) {
            assert(hasOnlyNoneOwnershipIncomingValues(phi2));
            phi2->replaceAllUsesWith(phi1);
            eraseOwnedPhiArgument(block, arg2Idx);
          } else {
            phi2->replaceAllUsesWith(phi1);
            erasePhiArgument(block, arg2Idx);
          }
        } else {
          arg2->replaceAllUsesWith(arg1);
          erasePhiArgument(block, arg2Idx);
        }
        changed = true;
      } else {
        ++arg2Idx;
      }
    }
  }
  return changed;
}

bool RedundantPhiEliminationPass::valuesAreEqual(SILValue val1, SILValue val2) {

  // Again, avoid running into quadratic behavior in case of cycles or long
  // chains of instructions. This limit is practically never exceeded.
  unsigned maxNumberOfChecks = 16;

  SmallVector<std::pair<SILValue, SILValue>, 8> workList;
  llvm::SmallSet<std::pair<SILValue, SILValue>, 16> handled;
  
  workList.push_back({val1, val2});
  handled.insert({val1, val2});

  while (!workList.empty()) {
  
    if (handled.size() > maxNumberOfChecks)
      return false;
  
    auto valuePair = workList.pop_back_val();
    SILValue val1 = valuePair.first;
    SILValue val2 = valuePair.second;

    // The trivial case.
    if (val1 == val2)
      continue;
 
    if (val1->getKind() != val2->getKind())
      return false;

    if (auto *arg1 = dyn_cast<SILPhiArgument>(val1)) {
      auto *arg2 = cast<SILPhiArgument>(val2);
      SILBasicBlock *argBlock = arg1->getParent();
      if (argBlock != arg2->getParent())
        return false;
      if (arg1->getType() != arg2->getType())
        return false;

      // Don't optimize if we have a phi arg with @guaranteed ownership.
      // Such a phi arg will be redundant only if all the incoming values have
      // none ownership.
      // In that case, we maybe able to eliminate the @guaranteed phi arg, by
      // creating a new borrow scope for the redundant @guaranteed phi arg, and
      // re-writing all the consuming uses in a way the new borrow scope is
      // within the borrow scope of its operand. This is not handled currently.
      if (arg1->getOwnershipKind() == OwnershipKind::Guaranteed ||
          arg2->getOwnershipKind() == OwnershipKind::Guaranteed) {
        return false;
      }
      // All incoming phi values must be equal.
      for (SILBasicBlock *pred : argBlock->getPredecessorBlocks()) {
        SILValue incoming1 = arg1->getIncomingPhiValue(pred);
        SILValue incoming2 = arg2->getIncomingPhiValue(pred);
        if (!incoming1 || !incoming2)
          return false;

        if (handled.insert({incoming1, incoming2}).second)
          workList.push_back({incoming1, incoming2});
      }
      continue;
    }
    
    if (auto *inst1 = dyn_cast<SingleValueInstruction>(val1)) {
      // Bail if the instructions have any side effects.
      if (inst1->getMemoryBehavior() != SILInstruction::MemoryBehavior::None)
        return false;

      // Allocation instructions are defined to have no side-effects.
      // Two allocations (even if the instruction look the same) don't define
      // the same value.
      if (isa<AllocationInst>(inst1))
        return false;

      auto *inst2 = cast<SingleValueInstruction>(val2);

      // Compare the operands by putting them on the worklist.
      if (!inst1->isIdenticalTo(inst2, [&](SILValue op1, SILValue op2) {
            if (handled.insert({op1, op2}).second)
              workList.push_back({op1, op2});
            return true;
          })) {
        return false;
      }
      continue;
    }
    
    return false;
  }

  return true;
}

/// Replaces struct phi-arguments by a struct field.
///
/// If only a single field of a struct phi-argument is used, replace the
/// argument by the field value.
///
/// \code
///     br bb(%str)
///   bb(%phi):
///     %f = struct_extract %phi, #Field // the only use of %phi
///     use %f
/// \endcode
///
/// is replaced with
///
/// \code
///     %f = struct_extract %str, #Field
///     br bb(%f)
///   bb(%phi):
///     use %phi
/// \endcode
///
/// This also works if the phi-argument is in a def-use cycle.
///
/// TODO: Handle tuples (but this is not so important).
///
/// The PhiExpansionPass is not part of SimplifyCFG because
/// * no other SimplifyCFG optimization depends on it.
/// * compile time: it doesn't need to run every time SimplifyCFG runs.
///
class PhiExpansionPass : public SILFunctionTransform {
public:
  PhiExpansionPass() {}

  void run() override;

private:
  bool optimizeArg(SILPhiArgument *initialArg);
};

void PhiExpansionPass::run() {
  SILFunction *F = getFunction();
  if (!F->shouldOptimize())
    return;

  LLVM_DEBUG(llvm::dbgs() << "*** PhiReduction on function: "
                          << F->getName() << " ***\n");

  bool changed = false;
  for (SILBasicBlock &block : *getFunction()) {
    for (auto argAndIdx : enumerate(block.getArguments())) {
      if (!argAndIdx.value()->isPhi())
        continue;
      
      unsigned idx = argAndIdx.index();
      
      // Try multiple times on the same argument to handle nested structs.
      while (optimizeArg(cast<SILPhiArgument>(block.getArgument(idx)))) {
        changed = true;
      }
    }
  }

  if (changed) {
    invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
}

bool PhiExpansionPass::optimizeArg(SILPhiArgument *initialArg) {
  llvm::SmallVector<const SILPhiArgument *, 8> collectedPhiArgs;
  llvm::SmallPtrSet<const SILPhiArgument *, 8> handled;
  collectedPhiArgs.push_back(initialArg);
  handled.insert(initialArg);

  VarDecl *field = nullptr;
  SILType newType;
  Optional<SILLocation> loc;
  
  // First step: collect all phi-arguments which can be transformed.
  unsigned workIdx = 0;
  while (workIdx < collectedPhiArgs.size()) {
    const SILArgument *arg = collectedPhiArgs[workIdx++];
    for (Operand *use : arg->getUses()) {
      SILInstruction *user = use->getUser();
      if (isa<DebugValueInst>(user))
        continue;
      
      if (auto *extr = dyn_cast<StructExtractInst>(user)) {
        if (field && extr->getField() != field)
          return false;
        field = extr->getField();
        newType = extr->getType();
        loc = extr->getLoc();
        continue;
      }
      if (auto *branch = dyn_cast<BranchInst>(user)) {
        const SILPhiArgument *destArg = branch->getArgForOperand(use);
        assert(destArg);
        if (handled.insert(destArg).second)
          collectedPhiArgs.push_back(destArg);
        continue;
      }
      if (auto *branch = dyn_cast<CondBranchInst>(user)) {
        const SILPhiArgument *destArg = branch->getArgForOperand(use);

        // destArg is null if the use is the condition and not a block argument.
        if (!destArg)
          return false;

        if (handled.insert(destArg).second)
          collectedPhiArgs.push_back(destArg);
        continue;
      }
      // An unexpected use -> bail.
      return false;
    }
  }

  if (!field)
    return false;

  // Second step: do the transformation.
  for (const SILPhiArgument *arg : collectedPhiArgs) {
    SILBasicBlock *block = arg->getParent();
    SILArgument *newArg = block->replacePhiArgumentAndReplaceAllUses(
                             arg->getIndex(), newType, arg->getOwnershipKind());

    // First collect all users, then do the transformation.
    // We don't want to modify the use list while iterating over it.
    llvm::SmallVector<DebugValueInst *, 8> debugValueUsers;
    llvm::SmallVector<StructExtractInst *, 8> structExtractUsers;

    for (Operand *use : newArg->getUses()) {
      SILInstruction *user = use->getUser();
      if (auto *dvi = dyn_cast<DebugValueInst>(user)) {
        debugValueUsers.push_back(dvi);
        continue;
      }
      if (auto *sei = dyn_cast<StructExtractInst>(user)) {
        structExtractUsers.push_back(sei);
        continue;
      }
      // Branches are handled below by handling incoming phi operands.
      assert(isa<BranchInst>(user) || isa<CondBranchInst>(user));
    }
  
    for (DebugValueInst *dvi : debugValueUsers) {
      dvi->eraseFromParent();
    }
    for (StructExtractInst *sei : structExtractUsers) {
      sei->replaceAllUsesWith(sei->getOperand());
      sei->eraseFromParent();
    }

    // "Move" the struct_extract to the predecessors.
    llvm::SmallVector<Operand *, 8> incomingOps;
    bool success = newArg->getIncomingPhiOperands(incomingOps);
    (void)success;
    assert(success && "could not get all incoming phi values");

    for (Operand *op : incomingOps) {
      // Did we already handle the operand?
      if (op->get()->getType() == newType)
        continue;

      SILInstruction *branchInst = op->getUser();
      SILBuilder builder(branchInst);
      auto *strExtract = builder.createStructExtract(loc.getValue(),
                                                     op->get(), field, newType);
      op->set(strExtract);
    }
  }
  return true;
}

} // end anonymous namespace

SILTransform *swift::createRedundantPhiElimination() {
  return new RedundantPhiEliminationPass();
}

SILTransform *swift::createPhiExpansion() {
  return new PhiExpansionPass();
}
