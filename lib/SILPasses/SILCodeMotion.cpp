//===- SILCodeMotion.cpp - Code Motion Optimizations ----------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "codemotion"
#include "swift/SILPasses/Passes.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SIL/Projection.h"
#include "swift/SILPasses/Utils/Local.h"
#include "swift/SILPasses/Transforms.h"
#include "swift/SILAnalysis/AliasAnalysis.h"
#include "swift/SILAnalysis/ARCAnalysis.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/ScopedHashTable.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/RecyclingAllocator.h"

STATISTIC(NumSunk,   "Number of instructions sunk");

using namespace swift;
using namespace swift::arc;

static const int SinkSearchWindow = 6;

/// \brief Returns True if we can sink this instruction to another basic block.
static bool canSinkInstruction(SILInstruction *Inst) {
  return Inst->use_empty() && !isa<TermInst>(Inst);
}

/// \brief Returns true if this instruction is a skip barrier, which means that
/// we can't sink other instructions past it.
static bool isSinkBarrier(SILInstruction *Inst) {
  // We know that some calls do not have side effects.
  if (const ApplyInst *AI = dyn_cast<ApplyInst>(Inst))
    if (BuiltinFunctionRefInst *FR =
        dyn_cast<BuiltinFunctionRefInst>(AI->getCallee()))
      return !isSideEffectFree(FR);

  if (isa<TermInst>(Inst))
    return false;

  if (Inst->mayHaveSideEffects())
    return true;

  return false;
}

/// \brief Search for an instruction that is identical to \p Iden by scanning
/// \p BB starting at the end of the block, stopping on sink barriers.
SILInstruction *findIdenticalInBlock(SILBasicBlock *BB, SILInstruction *Iden) {
  int SkipBudget = SinkSearchWindow;

  SILBasicBlock::iterator InstToSink = BB->getTerminator();

  while (SkipBudget) {
    // If we found a sinkable instruction that is identical to our goal
    // then return it.
    if (canSinkInstruction(InstToSink) && Iden->isIdenticalTo(InstToSink)) {
      DEBUG(llvm::dbgs() << "Found an identical instruction.");
      return InstToSink;
    }

    // If this instruction is a skip-barrier end the scan.
    if (isSinkBarrier(InstToSink))
      return nullptr;

    // If this is the first instruction in the block then we are done.
    if (InstToSink == BB->begin())
      return nullptr;

    SkipBudget--;
    InstToSink = std::prev(InstToSink);
    DEBUG(llvm::dbgs() << "Continuing scan. Next inst: " << *InstToSink);
  }

  return nullptr;
}

// Try to sink values from the Nth argument \p ArgNum.
static bool sinkArgument(SILBasicBlock *BB, unsigned ArgNum) {
  assert(ArgNum < BB->getNumBBArg() && "Invalid argument");

  // Find the first predecessor, the first terminator and the Nth argument.
  SILBasicBlock *FirstPred = *BB->pred_begin();
  TermInst *FirstTerm = FirstPred->getTerminator();
  auto FirstPredArg = FirstTerm->getOperand(ArgNum);
  SILInstruction *FSI = dyn_cast<SILInstruction>(FirstPredArg);

  // The list of identical instructions.
  SmallVector<SILValue, 8> Clones;
  Clones.push_back(FirstPredArg);

  // We only move instructions with a single use.
  if (!FSI || !FSI->hasOneUse())
    return false;

  // Don't move instructions that are sensitive to their location.
  if (FSI->mayHaveSideEffects())
    return false;

  // Check if the Nth argument in all predecessors is identical.
  for (auto P : BB->getPreds()) {
    if (P == FirstPred)
      continue;

    // Only handle branch or conditional branch instructions.
    TermInst *TI = P->getTerminator();
    if (!isa<BranchInst>(TI) && !isa<CondBranchInst>(TI))
      return false;

    // Find the Nth argument passed to BB.
    SILValue Arg = TI->getOperand(ArgNum);
    SILInstruction *SI = dyn_cast<SILInstruction>(Arg);
    if (SI && SI->hasOneUse() && SI->isIdenticalTo(FSI)) {
      Clones.push_back(SI);
      continue;
    }

    // Arguments are different.
    return false;
  }

  if (!FSI)
    return false;

  SILValue Undef = SILUndef::get(FirstPredArg.getType(), BB->getModule());

  // Sink one of the copies of the instruction.
  FirstPredArg.replaceAllUsesWith(Undef);
  FSI->moveBefore(BB->begin());
  SILValue(BB->getBBArg(ArgNum)).replaceAllUsesWith(FirstPredArg);

  // The argument is no longer in use. Replace all incoming inputs with undef
  // and try to delete the instruction.
  for (auto S : Clones)
    if (S.getDef() != FSI) {
      S.replaceAllUsesWith(Undef);
      auto DeadArgInst = cast<SILInstruction>(S.getDef());
      recursivelyDeleteTriviallyDeadInstructions(DeadArgInst);
    }

  return true;
}

/// Try to sink identical arguments coming from multiple predecessors.
static bool sinkArgumentsFromPredecessors(SILBasicBlock *BB) {
  if (BB->pred_empty() || BB->getSinglePredecessor())
    return false;

  // This block must be the only successor of all the predecessors.
  for (auto P : BB->getPreds())
    if (P->getSingleSuccessor() != BB)
      return false;

  // Try to sink values from each of the arguments to the basic block.
  bool Changed = false;
  for (int i = 0, e = BB->getNumBBArg(); i < e; ++i)
    Changed |= sinkArgument(BB, i);

  return Changed;
}

static bool sinkCodeFromPredecessors(SILBasicBlock *BB) {
  bool Changed = false;
  if (BB->pred_empty())
    return Changed;

  // This block must be the only successor of all the predecessors.
  for (auto P : BB->getPreds())
    if (P->getSingleSuccessor() != BB)
      return Changed;

  SILBasicBlock *FirstPred = *BB->pred_begin();
  // The first Pred must have at least one non-terminator.
  if (FirstPred->getTerminator() == FirstPred->begin())
    return Changed;

  DEBUG(llvm::dbgs() << " Sinking values from predecessors.\n");

  unsigned SkipBudget = SinkSearchWindow;

  // Start scanning backwards from the terminator.
  SILBasicBlock::iterator InstToSink = FirstPred->getTerminator();

  while (SkipBudget) {
    DEBUG(llvm::dbgs() << "Processing: " << *InstToSink);

    // Save the duplicated instructions in case we need to remove them.
    SmallVector<SILInstruction *, 4> Dups;

    if (canSinkInstruction(InstToSink)) {
      // For all preds:
      for (auto P : BB->getPreds()) {
        if (P == FirstPred)
          continue;

        // Search the duplicated instruction in the predecessor.
        if (SILInstruction *DupInst = findIdenticalInBlock(P, InstToSink)) {
          Dups.push_back(DupInst);
        } else {
          DEBUG(llvm::dbgs() << "Instruction mismatch.\n");
          Dups.clear();
          break;
        }
      }

      // If we found duplicated instructions, sink one of the copies and delete
      // the rest.
      if (Dups.size()) {
        DEBUG(llvm::dbgs() << "Moving: " << *InstToSink);
        InstToSink->moveBefore(BB->begin());
        Changed = true;
        for (auto I : Dups) {
          I->replaceAllUsesWith(InstToSink);
          I->eraseFromParent();
          NumSunk++;
        }

        // Restart the scan.
        InstToSink = FirstPred->getTerminator();
        DEBUG(llvm::dbgs() << "Restarting scan. Next inst: " << *InstToSink);
        continue;
      }
    }

    // If this instruction was a barrier then we can't sink anything else.
    if (isSinkBarrier(InstToSink)) {
      DEBUG(llvm::dbgs() << "Aborting on barrier: " << *InstToSink);
      return Changed;
    }

    // This is the first instruction, we are done.
    if (InstToSink == FirstPred->begin()) {
      DEBUG(llvm::dbgs() << "Reached the first instruction.");
      return Changed;
    }

    SkipBudget--;
    InstToSink = std::prev(InstToSink);
    DEBUG(llvm::dbgs() << "Continuing scan. Next inst: " << *InstToSink);
  }

  return Changed;
}

static void createRefCountOpForPayload(SILBuilder &Builder, SILInstruction *I,
                                       EnumElementDecl *EnumDecl) {
  // If we do not have any argument, there is nothing to do... bail...
  if (!EnumDecl->hasArgumentType())
    return;

  // Otherwise, create a UEDI for our payload.
  SILModule &Mod = I->getModule();
  SILType ArgType =
      I->getOperand(0).getType().getEnumElementType(EnumDecl, Mod);
  auto *UEDI = Builder.createUncheckedEnumData(I->getLoc(), I->getOperand(0),
                                               EnumDecl, ArgType);
  if (isa<RetainValueInst>(I)) {
    Builder.createRetainValue(I->getLoc(), UEDI);
    return;
  }

  Builder.createReleaseValue(I->getLoc(), UEDI);
}

/// Sink retain_value, release_value before switch_enum to be retain_value,
/// release_value on the payload of the switch_enum in the destination BBs. We
/// only do this if the destination BBs have only the switch enum as its
/// predecessor.
static bool tryToSinkRefCountAcrossSwitch(SwitchEnumInst *S, SILInstruction *I,
                                  AliasAnalysis *AA) {
  // If this instruction is not a retain_value, there is nothing left for us to
  // do... bail...
  if (!isa<RetainValueInst>(I))
    return false;

  SILValue Ptr = I->getOperand(0);

  // If the retain value's argument is not the switch's argument, we can't do
  // anything with our simplistic analysis... bail...
  if (Ptr != S->getOperand())
    return false;

  // Next go over all instructions after I in the basic block. If none of them
  // can decrement our ptr value, we can move the retain over the ref count
  // inst. If any of them do potentially decrement the ref count of Ptr, we can
  // not move it.
  SILBasicBlock::iterator II = I;
  ++II;
  for (; &*II != S; ++II) {
    if (canDecrementRefCount(&*II, Ptr, AA)) {
      return false;
    }
  }

  SILBuilder Builder(S);

  // Ok, we have a ref count instruction, sink it!
  for (unsigned i = 0, e = S->getNumCases(); i != e; ++i) {
    auto Case = S->getCase(i);
    EnumElementDecl *Enum = Case.first;
    SILBasicBlock *Succ = Case.second;
    Builder.setInsertionPoint(&*Succ->begin());
    createRefCountOpForPayload(Builder, I, Enum);
  }

  I->eraseFromParent();
  NumSunk++;
  return true;
}

static bool tryToSinkRefCountInst(SILInstruction *T, SILInstruction *I,
                                  AliasAnalysis *AA) {
  if (auto *S = dyn_cast<SwitchEnumInst>(T))
    return tryToSinkRefCountAcrossSwitch(S, I, AA);

  // We currently handle checked_cast_br and cond_br.
  if (!isa<CheckedCastBranchInst>(T) && !isa<CondBranchInst>(T))
    return false;

  if (!isa<StrongRetainInst>(I))
    return false;

  SILValue Ptr = I->getOperand(0);
  SILBasicBlock::iterator II = I;
  ++II;
  for (; &*II != T; ++II) {
    if (canDecrementRefCount(&*II, Ptr, AA))
      return false;
  }

  SILBuilder Builder(T);

  // Ok, we have a ref count instruction, sink it!
  for (auto &Succ : T->getParent()->getSuccs()) {
    SILBasicBlock *SuccBB = Succ.getBB();
    Builder.setInsertionPoint(&*SuccBB->begin());
    Builder.createStrongRetain(I->getLoc(), Ptr);
  }

  I->eraseFromParent();
  NumSunk++;
  return true;
}

/// Sink retains to successors if possible. We only do this if the successors
/// have only one predecessor.
static bool sinkRetainToSuccessors(SILBasicBlock *BB, AliasAnalysis *AA) {
  SILInstruction *S = BB->getTerminator();

  // Make sure that each one of our successors only has one predecessor,
  // us. If that condition is not true, bail...
  for (auto &Succ : BB->getSuccs()) {
    SILBasicBlock *SuccBB = Succ.getBB();
    if (!SuccBB || !SuccBB->getSinglePredecessor())
      return false;
  }

  // Ok, we can perform this transformation... We bail immediately if we do not
  // have
  bool Changed = false;
  SILBuilder Builder(BB);

  SILBasicBlock::iterator SI = S, SE = BB->begin();
  if (SI == SE)
    return false;

  SI = std::prev(SI);

  while (SI != SE) {
    SILInstruction *Inst = &*SI;
    SI = std::prev(SI);
    if (tryToSinkRefCountInst(S, Inst, AA))
      Changed = true;
  }

  return Changed | tryToSinkRefCountInst(S, &*SI, AA);
}

namespace {
class SILCodeMotion : public SILFunctionTransform {

  /// The entry point to the transformation.
  void run() {
    SILFunction &F = *getFunction();
    AliasAnalysis *AA = getAnalysis<AliasAnalysis>();

    DEBUG(llvm::dbgs() << "***** CodeMotion on function: " << F.getName() <<
          " *****\n");

    // Sink duplicated code from predecessors.
    bool Changed = false;
    for (auto &BB : F) {
      Changed |= sinkCodeFromPredecessors(&BB);
      Changed |= sinkArgumentsFromPredecessors(&BB);
      Changed |= sinkRetainToSuccessors(&BB, AA);
    }

    if (Changed)
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }

  StringRef getName() override { return "SIL Code Motion"; }
};
} // end anonymous namespace

SILTransform *swift::createCodeMotion() {
  return new SILCodeMotion();
}
