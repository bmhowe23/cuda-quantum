/*******************************************************************************
 * Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#include "PassDetails.h"
#include "cudaq/Frontend/nvqpp/AttributeNames.h"
#include "cudaq/Optimizer/Dialect/CC/CCOps.h"
#include "cudaq/Optimizer/Dialect/Quake/QuakeDialect.h"
#include "cudaq/Optimizer/Dialect/Quake/QuakeTypes.h"
#include "cudaq/Optimizer/Transforms/Passes.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"

#define DEBUG_TYPE "dep-analysis"

using namespace mlir;

#define RAW(X) quake::X
#define RAW_MEASURE_OPS MEASURE_OPS(RAW)
#define RAW_GATE_OPS GATE_OPS(RAW)
#define RAW_QUANTUM_OPS QUANTUM_OPS(RAW)

//===----------------------------------------------------------------------===//
// Generated logic
//===----------------------------------------------------------------------===//
namespace cudaq::opt {
#define GEN_PASS_DEF_DEPENDENCYANALYSIS
#include "cudaq/Optimizer/Transforms/Passes.h.inc"
} // namespace cudaq::opt

namespace {
// TODO: Someday, it would probably make sense to make VirtualQIDs and
// PhysicalQIDs be data structures with metadata, not just integer
// identifiers. Some useful metadata would include the lifetime,
// which graph they belong to, where the DependencyNode representing
// their allocation is, etc...

/// A `PhysicalQID` is an index that will be used when generating
/// `quake.borrow_wire`s. It represents a physical wire.
typedef std::size_t PhysicalQID;

// TODO: Unfortunately, `contractAllocsPass` will currently duplicate
// VirtualQIDs in the then and else blocks of an if, so they are only
// unique per path, not per program, which could lead to multiple
// different wires with the same VirtualQID after lifting allocs.

/// A `VirtualQID` is a (mostly) unique identifier for a virtual wire.
/// It is a handy way to refer to a specific virtual wire.
typedef std::size_t VirtualQID;

/// Given a `quake` operation and an result index for a wire result,
/// returns the corresponding operand index for the wire input.
std::size_t getOperandIDXFromResultIDX(std::size_t resultidx, Operation *op) {
  // The results for a measure are `(!quake.measure, !quake.wire)`
  if (isa<RAW_MEASURE_OPS>(op))
    return 0;
  if (isa<quake::SwapOp>(op))
    return (resultidx == 0 ? 1 : 0);
  // Currently, all classical operands precede all quantum operands
  for (auto type : op->getOperandTypes()) {
    if (!quake::isQuantumType(type))
      resultidx++;
    else
      break;
  }
  return resultidx;
}

/// Given a `quake` operation and an operand index for a wire input,
/// returns the corresponding result index for the wire result.
/// This is almost the inverse of `getOperandIDXFromResultIDX`,
/// and is the inverse if `quake.measure` results are ignored.
std::size_t getResultIDXFromOperandIDX(std::size_t operand_idx, Operation *op) {
  // The results for a measure are `(!quake.measure, !quake.wire)`
  if (isa<RAW_MEASURE_OPS>(op))
    return 1;
  if (isa<quake::SwapOp>(op))
    return (operand_idx == 0 ? 1 : 0);
  // Currently, all classical operands precede all quantum operands
  for (auto type : op->getOperandTypes()) {
    if (!quake::isQuantumType(type))
      operand_idx--;
    else
      break;
  }
  return operand_idx;
}

/// Represents a qubit lifetime from the first cycle it is in use
/// to the last cycle it is in use (inclusive).
class LifeTime {
protected:
  unsigned begin;
  unsigned end;

public:
  LifeTime(unsigned begin, unsigned end) : begin(begin), end(end) {
    assert(end >= begin && "invalid lifetime");
  };

  /// Returns true if \p this is entirely after \p other
  bool isAfter(LifeTime other) { return begin > other.end; }

  bool isOverlapping(LifeTime other) {
    return !isAfter(other) && !other.isAfter(*this);
  }

  /// Calculates the distance between \p this and \p other,
  /// in terms of the # of cycles between the end of the earlier
  /// LifeTime and the beginning of the later LifeTime.
  /// Returns 0 if the LifeTimes overlap.
  unsigned distance(LifeTime other) {
    if (isOverlapping(other))
      return 0;
    return std::max(begin, other.begin) - std::min(end, other.end);
  }

  /// Modifies \p this LifeTime to be inclusive of \p other
  /// and any cycles between \p this and \p other.
  void combine(LifeTime other) {
    begin = std::min(begin, other.begin);
    end = std::max(end, other.end);
  }

  unsigned getBegin() { return begin; }
  unsigned getEnd() { return end; }
};

/// Contains LifeTime information for allocating physical qubits for
/// VirtualQIDs.
class LifeTimeAnalysis {
private:
  StringRef name;
  SmallVector<std::optional<LifeTime>> lifetimes;

  /// Given a candidate lifetime, tries to find a qubit to reuse,
  /// minimizing the distance between the lifetime of the existing
  /// qubit and \p lifetime, and otherwise allocates a new qubit
  PhysicalQID allocatePhysical(LifeTime lifetime) {
    std::optional<PhysicalQID> best_reuse = std::nullopt;
    std::optional<PhysicalQID> empty = std::nullopt;
    unsigned best_distance = INT_MAX;

    for (unsigned i = 0; i < lifetimes.size(); i++) {
      if (!lifetimes[i]) {
        empty = i;
        continue;
      }

      auto other = lifetimes[i].value();
      auto distance = lifetime.distance(other);
      if (!lifetime.isOverlapping(other) && distance < best_distance) {
        best_reuse = i;
        best_distance = distance;
      }
    }

    // Reuse a qubit based on its lifetime in the same scope
    if (best_reuse) {
      auto physical = best_reuse.value();
      lifetimes[physical]->combine(lifetime);
      return physical;
    }

    // Reuse a qubit without a lifetime (used in a different frame)
    if (empty) {
      auto physical = empty.value();
      lifetimes[physical] = lifetime;
      return physical;
    }

    // Fall back: allocate a new qubit
    lifetimes.push_back(lifetime);
    return lifetimes.size() - 1;
  }

public:
  LifeTimeAnalysis(StringRef name) : name(name), lifetimes() {}

  /// Given a candidate lifetime, tries to find a qubit to reuse,
  /// minimizing the distance between the lifetime of the existing
  /// qubit and \p lifetime, and otherwise allocates a new qubit.
  PhysicalQID allocatePhysical(VirtualQID qid, LifeTime lifetime) {
    auto phys = allocatePhysical(lifetime);
    return phys;
  }

  /// Clears the lifetime information (erasing in-use lifetimes),
  /// and returns a set of all physical qubits currently in use.
  ///
  /// This is meant to be called by an `IfDependencyNode` after
  /// performing qubit allocation in the inner blocks, so that the
  /// inner blocks can perform qubit allocation in a clean state, but
  /// the parent `IfDependencyNode` can capture the inner allocation
  /// information.
  SetVector<PhysicalQID> clearFrame() {
    SetVector<PhysicalQID> frame;
    for (uint i = 0; i < lifetimes.size(); i++) {
      if (lifetimes[i]) {
        frame.insert(i);
        lifetimes[i] = std::nullopt;
      }
    }
    return frame;
  }

  /// Sets the lifetime for \p phys to \p lifetime, essentially
  /// reallocating \p phys (used by an `IfDependencyNode` to
  /// mark a qubit as in use for the entirety of the `if`).
  void reallocatePhysical(PhysicalQID phys, LifeTime lifetime) {
    assert(phys < lifetimes.size() && "Illegal qubit to reallocate!");
    assert(!lifetimes[phys] && "Cannot reallocate qubit still allocated!");
    lifetimes[phys] = lifetime;
  }

  std::size_t getCount() { return lifetimes.size(); }

  void print() {
    llvm::outs() << "# qubits: " << getCount() << ", cycles: ";
    for (std::size_t i = 0; i < lifetimes.size(); i++)
      if (lifetimes[i])
        llvm::outs() << lifetimes[i].value().getBegin() << " - "
                     << lifetimes[i].value().getEnd() << " ";
      else
        llvm::outs() << "unused ";
    llvm::outs() << "\n";
  }

  StringRef getName() { return name; }
};

class DependencyGraph;

/// A DependencyNode represents an MLIR value or operation with attached
/// metadata. Most importantly, it captures dependency relations between quake
/// operations on wires, which is used for scheduling, lifetime analysis,
/// allocating physical qubits, lifting optimizations, and code generation.
///
/// There is a family of DependencyNodes, based on what types of MLIR
/// values/operations they represent: The most common type of DependencyNode,
/// the OpDependencyNode, represents a quantum gate operation, or a classical
/// operation. ArgDependencyNode represents a block argument. InitDependencyNode
/// and RootDependencyNode represent the allocation/de-allocation of a quake
/// wire, respectively. A TerminatorDependencyNode represents a block
/// terminator, so a bit of care must be taken to ensure that it is always the
/// last operation in a block during code generation. An IfDependencyNode
/// represents an if, and therefore contains information about the then and else
/// blocks. An IfDependencyNode is treated as a "rectangle" by the analysis,
/// where the analysis of the outside scope does not look inside the `if`
/// (though optimizations are free to do so, as long as the maintain the
/// boundary afterwards). Finally, a ShadowDependencyNode represents a
/// dependency on a quantum-dependent classical value from a higher scope than
/// the operation that depends on it. This is necessary to ensure that the `if`
/// the dependent operation is in depends on the classical value, and the
/// operation inside the `if` can instead depend on the shadow dependency from
/// the `if`, ensuring that the boundaries of the `if` are properly maintained.
///
/// There are three types of "containers" for DependencyNodes:
/// A DependencyBlock represents an MLIR block, with ArgDependencyNodes
/// representing the linear block arguments (quake wires), a DependencyGraph
/// representing the block's body, and the terminator for the block. A
/// DependencyGraph is a DAG consisting of DependencyNodes somehow related by
/// interaction. It contains useful metadata and functions for reasoning about
/// and manipulating the DAG. Finally, an IfDependencyNode contains a
/// DependencyBlock each for the then and else branches.
class DependencyNode {
  friend class DependencyGraph;
  friend class OpDependencyNode;
  friend class IfDependencyNode;
  friend class ArgDependencyNode;
  friend class RootDependencyNode;
  friend class TerminatorDependencyNode;
  friend class InitDependencyNode;

public:
  /// A DependencyEdge is a dependency on a specific result from a specific
  /// node. It also contains useful metadata, such as the wire qid/qubit the
  /// edge represents (if applicable), and the underlying MLIR value the edge
  /// represents (through `getValue`).
  struct DependencyEdge {
  public:
    DependencyNode *node;
    // If a given dependency appears multiple times,
    // (e.g., multiple results of the dependency are used by this node),
    // it is important to know which result from the dependency
    // corresponds to which operand.
    // Otherwise, the dependency will be code gen'ed first, and it will
    // be impossible to know (e.g., which result is a control and which is a
    // target). Resultidx tracks this information.
    std::size_t resultidx;
    std::optional<VirtualQID> qid;
    std::optional<PhysicalQID> qubit;

    DependencyEdge() : node(nullptr), resultidx(INT_MAX), qid(std::nullopt) {}

    DependencyEdge(DependencyNode *node, std::size_t resultidx)
        : node(node), resultidx(resultidx) {
      assert(node && "DependencyEdge: node cannot be null");
      qid = node->getQIDForResult(resultidx);
    }

    /// Returns the underlying DependencyNode * without attached metadata
    DependencyNode *operator->() { return node; }

    /// Returns the value represented by this DependencyEdge
    Value getValue() { return node->getResult(resultidx); }
  };

protected:
  // Currently, successors are unordered, as any operation with non-linear
  // results (e.g., `mz`) does not have a known # of successors.
  SetVector<DependencyNode *> successors;
  // Dependencies are in the order of operands, this ordering is relied upon in
  // the optimizations and during code generation
  SmallVector<DependencyEdge> dependencies;
  // The set of virtual wires flowing through this node.
  // TODO: it would probably make sense to have a similar tracking of physical
  // wires here.
  SetVector<VirtualQID> qids;
  std::optional<unsigned> cycle = std::nullopt;
  bool hasCodeGen = false;
  unsigned height;

  virtual void printNode() = 0;

  void printSubGraph(int tabIndex) {
    for (int i = 0; i < tabIndex; i++) {
      llvm::outs() << "\t";
    }

    printNode();

    for (auto dependency : dependencies)
      dependency->printSubGraph(tabIndex + 1);
  }

  /// Returns the MLIR value representing the result of this node at \p
  /// resultidx
  virtual Value getResult(unsigned resultidx) = 0;

  /// Returns a name for the node to use for checking equivalence.
  // TODO: this is currently a little hacky and could be done a little better by
  // adding say an "equivalent node" function and overloading. For example,
  // block arguments can be checked by arg number, but currently the arg number
  // is part of the OpName for them. Allocs do have qid/qubit alloc info checked
  // explicitly in an overload of prefixEquivalent. Arithmetic constants are
  // handled by adding the constant value to the string, very inefficient...
  virtual std::string getOpName() = 0;

  /// Generates quake code for this node at the current insertion point in \p
  /// builder
  virtual void codeGen(OpBuilder &builder, LifeTimeAnalysis &set) = 0;

  /// Recalculates the height of this node
  virtual void updateHeight() {
    height = 0;
    for (auto edge : dependencies) {
      if (edge->getHeight() > height)
        height = edge->getHeight();
    }
    height += numTicks();
  }

public:
  DependencyNode() : successors(), dependencies({}), qids({}), height(0) {}

  virtual ~DependencyNode(){};

  /// Returns true if \p this is a graph root (has no successors, e.g., a wire
  /// de-alloc)
  virtual bool isRoot() { return successors.size() == 0; };
  /// Returns true if \p this is a graph leaf (has no dependencies, e.g., a wire
  /// alloc)
  virtual bool isLeaf() { return dependencies.size() == 0; };
  /// Returns true if \p this is not an operation which has an associated cycle
  /// cost
  virtual bool isSkip() { return numTicks() == 0; };
  /// Returns true if the associated value/operation is a quantum
  /// value/operation
  virtual bool isQuantumOp() = 0;
  /// Returns the number of cycles this node takes
  virtual unsigned numTicks() = 0;
  /// Returns true if and only if this is an InitDependencyNode
  virtual bool isAlloc() { return false; }
  /// Returns the height of this dependency node, based on the # of cycles it
  /// will take and the heights of its dependencies
  unsigned getHeight() { return height; };
  /// Prints this node and its dependencies to llvm::outs()
  void print() { printSubGraph(0); }
  /// Returns true if this node is a quantum operation/value or has a quantum
  /// operation as an ancestor. In fact, after inlining, Canonicalization, and
  /// CSE, this should only returns false for arithmetic constants.
  virtual bool isQuantumDependent() {
    if (isQuantumOp())
      return true;
    for (auto dependency : dependencies)
      if (dependency->isQuantumDependent())
        return true;
    return false;
  };
  /// Returns true if this node contains more dependency nodes inside of it
  /// (currently, this is only true of `IfDependencyNode`s).
  virtual bool isContainer() { return false; }

  /// Returns the index of the dependency for \p qid in this node, if such an
  /// index exists
  std::optional<std::size_t> getDependencyForQID(VirtualQID qid) {
    for (unsigned i = 0; i < dependencies.size(); i++)
      if (dependencies[i].qid == qid)
        return std::optional<std::size_t>(i);

    return std::nullopt;
  }

  /// Returns the immediate successor node for the wire represented by \p qid
  virtual DependencyNode *getSuccessorForQID(VirtualQID qid) {
    assert(qids.contains(qid) &&
           "Asking for a qid that doesn't flow through this operation!");
    for (auto successor : successors) {
      // Special case: ignore patch discriminate for a measure
      if (!successor->isQuantumOp())
        continue;

      auto idx = successor->getDependencyForQID(qid);
      // If the successor has a dependency for the given QID, ensure that the
      // dependency is actually on this node, otherwise the QID flows through
      // a different successor first, so this isn't the successor we're looking
      // for
      if (idx && successor->dependencies[idx.value()].node == this)
        return successor;
    }

    assert(false && "Couldn't find successor for linear type!");
  }

  /// Recursively find nodes scheduled at a given cycle
  SetVector<DependencyNode *>
  getNodesAtCycle(unsigned _cycle, SetVector<DependencyNode *> &seen) {
    SetVector<DependencyNode *> nodes;

    if (seen.contains(this))
      return nodes;

    seen.insert(this);

    if (!isSkip()) {
      assert(cycle.has_value() && "Trying to use cycle of unscheduled node");

      if (cycle.value() < _cycle)
        return nodes;
      else if (cycle.value() == _cycle) {
        nodes.insert(this);
        return nodes;
      }
    }

    for (auto dependency : dependencies)
      nodes.set_union(dependency->getNodesAtCycle(_cycle, seen));

    return nodes;
  }

  /// Returns true if \p this and \p other are equivalent nodes with equivalent
  /// dependencies.
  virtual bool prefixEquivalentTo(DependencyNode *other) {
    if (getOpName() != other->getOpName())
      return false;
    if (height != other->height)
      return false;
    if (dependencies.size() != other->dependencies.size())
      return false;
    for (unsigned i = 0; i < dependencies.size(); i++) {
      if (dependencies[i].qid != other->dependencies[i].qid) {
        if (!dependencies[i].qubit.has_value())
          return false;
        if (dependencies[i].qubit != other->dependencies[i].qubit)
          return false;
      }
      // TODO: I think the above nested check should be the same as in
      // postfixEquivalentTo, as in the following:
      /*
      if (dependencies[i].qubit != other->dependencies[i].qubit)
        return false;
      if (dependencies[i].qid != other->dependencies[i].qid)
        if (dependencies[i].qubit.has_value() ||
            other->dependencies[i].qubit.has_value())
          return false;*/
      if (!dependencies[i].node->prefixEquivalentTo(
              other->dependencies[i].node))
        return false;
    }
    return true;
  }

  /// Returns true if \p this and \p other are equivalent nodes with equivalent
  /// input wires, but without looking at dependencies.
  /// TODO: Currently, this does not handle classical values, which it should
  ///       really return false for as a first approximation.
  /// TODO: Arithmetic constants, aka DependencyNodes where
  ///       `isQuantumDependent()` is false, can be tested for equivalence,
  ///       however, testing quantum dependent values for equivalence is really
  ///       difficult in general, unless the classical values are actually
  ///       shadowed values from a higher scope.
  virtual bool postfixEquivalentTo(DependencyNode *other) {
    if (getOpName() != other->getOpName())
      return false;
    if (dependencies.size() != other->dependencies.size())
      return false;
    for (unsigned i = 0; i < dependencies.size(); i++) {
      if (dependencies[i].qubit != other->dependencies[i].qubit)
        return false;
      if (dependencies[i].qid != other->dependencies[i].qid)
        if (dependencies[i].qubit.has_value() ||
            other->dependencies[i].qubit.has_value())
          return false;
    }
    return true;
  }

  /// Returns the qubits that flow through this instruction
  virtual SetVector<PhysicalQID> getQubits() {
    return SetVector<PhysicalQID>();
  }

  /// Replaces every dependency on this node with the DependencyEdge \p other.
  /// This should only be used if it is known that any other node will have
  /// exactly one dependency on this node (the only real way to guarantee this
  /// is if this node has only one result).
  void replaceWith(DependencyEdge other) {
    for (auto successor : successors) {
      for (auto &dependency : successor->dependencies) {
        if (dependency.node == this) {
          dependency = other;
          other->successors.remove(this);
          other->successors.insert(successor);
        }
      }
    }
  }

  /// Recursively updates the dependency edges for \p qid to use \p qubit for
  /// this node and successors
  virtual void updateWithPhysical(VirtualQID qid, PhysicalQID qubit) {
    for (auto &dependency : dependencies) {
      if (dependency.qid && dependency.qid == qid) {
        dependency.qubit = qubit;
        break;
      }
    }

    for (auto successor : successors)
      if (successor->qids.contains(qid))
        successor->updateWithPhysical(qid, qubit);
  }

  /// Recursively replaces \p old_qid with \p new_qid for this node and its
  /// successors
  void updateQID(VirtualQID old_qid, VirtualQID new_qid) {
    qids.remove(old_qid);
    qids.insert(new_qid);

    auto idx = getDependencyForQID(old_qid);

    if (idx)
      dependencies[idx.value()].qid = new_qid;

    for (auto successor : successors)
      if (successor->qids.contains(old_qid))
        successor->updateQID(old_qid, new_qid);
  }

  /// If a wire's first and last use inside a block is in an `if`, move the
  /// alloc/de-alloc into both the then and else blocks (separately) of the
  /// `if`, which will make the lifetime analysis more accurate and may provide
  /// additional lifting opportunities. If the wire is not used in the then or
  /// else branch it is deleted after being moved in.
  ///
  /// This function works recursively outside-in, so wires are moved in
  /// (contracted) as far as possible.
  ///
  /// This function should only be called on DependencyNodes where
  /// `isContainer()` is true.
  virtual void contractAllocsPass() {
    assert(false &&
           "contractAllocPass can only be called on an IfDependencyNode");
  }

  /// Given a virtual wire and corresponding alloc/de-alloc nodes from a parent
  /// scope, moves the virtual wire into the then and else blocks (separately)
  /// of the `if`.
  ///
  /// This is used by contractAllocsPass, when `this->isContainer()` is true and
  /// this node is the first and last use of the virtual wire in the parent
  /// scope.
  virtual void lowerAlloc(DependencyNode *init, DependencyNode *root,
                          VirtualQID alloc) {
    assert(false && "lowerAlloc can only be called on an IfDependencyNode");
  }

  /// Recursively schedules nodes and performs lifetime analysis to allocate
  /// physical qubits for virtual wires, working inside out. For
  /// `IfDependencyNode`s, this means combining the physical qubit allocations
  /// of the then and else blocks, and then performing lifting optimizations,
  /// where common operations in the then and else blocks are lifted to the
  /// graph containing the `if`, hence the need to pass \p parent_graph
  ///
  /// This function should only be called on DependencyNodes where isContainer()
  /// is true.
  virtual void performAnalysis(LifeTimeAnalysis &set,
                               DependencyGraph *parent_graph) {
    assert(false &&
           "performAnalysis can only be called on an IfDependencyNode");
  }

  /// Remove this dependency node from the path for \p qid by replacing
  /// successor dependencies on \p qid with the relevant dependency from this
  /// node.
  virtual void eraseEdgeForQID(VirtualQID qid) = 0;

  virtual std::optional<VirtualQID> getQIDForResult(std::size_t resultidx) = 0;
};

/// An InitDependencyNode represents an allocation of virtual wire or physical
/// qubit (more concretely, a `quake.borrow_wire`). This node will always be a
/// leaf node, as it will never have dependencies.
/// TODO: The reason it doesn't derive from OpDependencyNode is historical and
/// doesn't apply anymore, but it's also not really clear that there would be
/// any benefit to deriving since it overloads a lot of the functions anyway.
class InitDependencyNode : public DependencyNode {
  friend class DependencyGraph;
  friend class IfDependencyNode;

protected:
  Value wire;
  std::optional<PhysicalQID> qubit = std::nullopt;

  void printNode() override {
    llvm::outs() << "Initial value for QID " << getQID();
    if (qubit)
      llvm::outs() << " -> phys: " << qubit.value();
    llvm::outs() << ": ";
    wire.dump();
  }

  Value getResult(unsigned resultidx) override {
    assert(resultidx == 0 && "Illegal resultidx");
    return wire;
  }

  std::string getOpName() override { return "init"; };

  /// Generates quake code for this node at the current insertion point in \p
  /// builder
  void codeGen(OpBuilder &builder, LifeTimeAnalysis &set) override {
    assert(qubit.has_value() && "Trying to codeGen a virtual allocation "
                                "without a physical qubit assigned!");
    auto wirety = quake::WireType::get(builder.getContext());
    auto alloc = builder.create<quake::BorrowWireOp>(
        builder.getUnknownLoc(), wirety, set.getName(), qubit.value());
    wire = alloc.getResult();
    hasCodeGen = true;
  }

  /// Assigns the physical qubit \p phys to this virtual wire, recursively
  /// updating all dependencies on this node
  void assignToPhysical(PhysicalQID phys) {
    qubit = phys;
    updateWithPhysical(getQID(), phys);
  }

public:
  InitDependencyNode(quake::BorrowWireOp op) : wire(op.getResult()) {
    // Lookup qid from op
    auto qid = op.getIdentity();
    qids.insert(qid);
  };

  // "Allocation" occurs statically in the base and adaptive profiles, so takes
  // no cycles
  unsigned numTicks() override { return 0; }
  bool isQuantumOp() override { return true; }

  /// Returns the qid for the virtual wire this node allocates
  VirtualQID getQID() { return qids.front(); }

  /// Returns the qubit for the physical wire this node allocates if assigned
  std::optional<PhysicalQID> getQubit() { return qubit; }

  ~InitDependencyNode() override {}

  bool isAlloc() override { return true; }

  bool prefixEquivalentTo(DependencyNode *other) override {
    if (!other->isAlloc())
      return false;

    auto other_init = static_cast<InitDependencyNode *>(other);

    // Two allocations are equivalent if they represent the same physical qubit.
    // TODO: if qids are made unique, this test can refer to qids if qubits are
    // not yet assigned (or even pointer equivalence in the meantime).
    return qubit && other_init->qubit && qubit == other_init->qubit;
  }

  void eraseEdgeForQID(VirtualQID qid) override {
    assert(false && "Can't call eraseEdgeForQID with an InitDependencyNode");
  }

  SetVector<PhysicalQID> getQubits() override {
    SetVector<PhysicalQID> qubits;
    if (qubit)
      qubits.insert(qubit.value());
    return qubits;
  }

  std::optional<VirtualQID> getQIDForResult(std::size_t resultidx) override {
    assert(resultidx == 0 && "Invalid resultidx");
    return std::optional(getQID());
  }
};

/// An OpDependencyNode represents a quantum or classical operation.
class OpDependencyNode : public DependencyNode {
  friend class IfDependencyNode;
  friend class ShadowDependencyNode;

protected:
  Operation *associated;
  bool quantumOp;

  virtual void printNode() override {
    llvm::outs() << "QIDs: ";
    bool printComma = false;
    for (auto qid : qids) {
      if (printComma)
        llvm::outs() << ", ";
      llvm::outs() << qid;
      printComma = true;
    }
    if (cycle.has_value())
      llvm::outs() << " @ " << cycle.value();
    llvm::outs() << " | " << height << ", " << numTicks() << " | ";
    associated->dump();
  }

  /// Returns the MLIR value representing the result of this node at \p
  /// resultidx
  Value getResult(unsigned resultidx) override {
    return associated->getResult(resultidx);
  }

  /// Returns a name for the node to use for checking equivalence.
  // TODO: this is currently a little hacky and could be done a little better by
  // adding say an "equivalent node" function and overloading. For example,
  // block arguments can be checked by arg number, but currently the arg number
  // is part of the OpName for them. Allocs do have qid/qubit alloc info checked
  // explicitly in an overload of prefixEquivalent. Arithmetic constants are
  // handled by adding the constant value to the string, very inefficient...
  std::string getOpName() override {
    if (isa<arith::ConstantOp>(associated)) {
      if (auto cstf = dyn_cast<arith::ConstantFloatOp>(associated)) {
        auto value = cstf.getValue().cast<FloatAttr>().getValueAsDouble();
        return std::to_string(value);
      } else if (auto cstidx = dyn_cast<arith::ConstantIndexOp>(associated)) {
        auto value = cstidx.getValue().cast<IntegerAttr>().getInt();
        return std::to_string(value);
      } else if (auto cstint = dyn_cast<arith::ConstantIntOp>(associated)) {
        auto value = cstint.getValue().cast<IntegerAttr>().getInt();
        return std::to_string(value);
      }
    }
    return associated->getName().getStringRef().str();
  };

  /// A helper to gather the MLIR values that will be used as operands for this
  /// operation when generating code, based on the dependencies of this node.
  SmallVector<mlir::Value> gatherOperands(OpBuilder &builder,
                                          LifeTimeAnalysis &set) {
    SmallVector<mlir::Value> operands(dependencies.size());
    for (std::size_t i = 0; i < dependencies.size(); i++) {
      auto dependency = dependencies[i];

      // Ensure classical values are available and that any allocs are added
      if (dependency->isSkip())
        dependency->codeGen(builder, set);

      assert(dependency->hasCodeGen &&
             "Generating code for successor before dependency");

      // Get relevant result from dependency's updated op
      // to use as the relevant operand
      operands[i] = dependency->getResult(dependency.resultidx);
    }

    return operands;
  }

  /// A helper to generate the quake code for this operation
  virtual void genOp(OpBuilder &builder, LifeTimeAnalysis &set) {
    auto oldOp = associated;
    auto operands = gatherOperands(builder, set);

    associated =
        Operation::create(oldOp->getLoc(), oldOp->getName(),
                          oldOp->getResultTypes(), operands, oldOp->getAttrs());
    associated->removeAttr("dnodeid");
    builder.insert(associated);
  }

  /// Generates quake code for this node at the current insertion point in \p
  /// builder using the dependencies of the node as operands.
  ///
  /// Classical constants will be duplicated everywhere they are used,
  /// while all quantum-dependent operations will only have code generated
  /// once.
  ///
  /// If this operation is a quantum operation, then all quantum-dependent
  /// dependencies must already have code generated for them. If this assumption
  /// doesn't hold, it is likely something going wrong with scheduling or the
  /// graph structure, but the error may only show up here.
  virtual void codeGen(OpBuilder &builder, LifeTimeAnalysis &set) override {
    if (hasCodeGen && isQuantumDependent())
      return;

    // Non-quake operations have code generated aggressively
    // This ensures that code gen is not too aggressive
    if (isSkip())
      for (auto dependency : dependencies)
        if (!dependency->hasCodeGen && dependency->isQuantumDependent())
          // Wait for quantum op dependency to be codeGen'ed
          return;

    genOp(builder, set);
    hasCodeGen = true;

    // Ensure classical values are generated
    for (auto successor : successors)
      if (successor->isSkip() && isQuantumDependent())
        successor->codeGen(builder, set);
  }

public:
  OpDependencyNode(Operation *op, SmallVector<DependencyEdge> _dependencies)
      : associated(op) {
    assert(op && "Cannot make dependency node for null op");
    assert(_dependencies.size() == op->getNumOperands() &&
           "Wrong # of dependencies to construct node");

    dependencies = _dependencies;

    quantumOp = isQuakeOperation(op);
    // TODO: quake.discriminate is currently the only operation in the quake
    // dialect that doesn't operate on wires. This will need to be updated if
    // that changes.
    if (isa<quake::DiscriminateOp>(op))
      quantumOp = false;

    height = 0;
    // Ingest dependencies, setting up metadata
    for (std::size_t i = 0; i < dependencies.size(); i++) {
      auto edge = dependencies[i];

      assert(edge->getResult(edge.resultidx) == associated->getOperand(i) &&
             "Dependency isn't actually a dependency!");
      // Add this as a successor to each dependency
      edge->successors.insert(this);

      // Update metadata
      if (edge.qid.has_value() && quantumOp)
        qids.insert(edge.qid.value());
    }

    updateHeight();
  };

  virtual ~OpDependencyNode() override {}

  /// Currently, all quantum operations are considered to take 1 cycle.
  // TODO: make the cycle time configurable per operation.
  virtual unsigned numTicks() override { return isQuantumOp() ? 1 : 0; }
  virtual bool isQuantumOp() override { return quantumOp; }

  unsigned getHeight() { return height; }

  virtual std::size_t getResultForDependency(std::size_t operandidx) {
    return getResultIDXFromOperandIDX(operandidx, associated);
  }

  virtual void eraseEdgeForQID(VirtualQID qid) override {
    assert(qids.contains(qid) && "Erasing edge for QID not in node!");
    auto successor = getSuccessorForQID(qid);
    auto out_idx = successor->getDependencyForQID(qid).value();
    auto in_idx = getDependencyForQID(qid).value();
    auto dependency = dependencies[in_idx];
    dependencies.erase(dependencies.begin() + in_idx);
    successor->dependencies[out_idx] = dependency;
    dependency->successors.insert(successor);

    bool remove = true;

    // Remove successor if it has no other dependencies on this
    for (auto dependency : successor->dependencies)
      if (dependency.node == this)
        remove = false;

    if (remove)
      successors.remove(successor);

    // Update successor's height after adding a new dependency
    // This won't fix the height recursively, but is key for lifting
    // as if a dependency was lifted, now the successor may be liftable
    successor->updateHeight();

    remove = true;
    for (auto edge : dependencies)
      if (edge.node == dependency.node)
        remove = false;

    // Only remove this as a successor from dependency if this was the last
    // QID from dependency we depended on
    if (remove)
      dependency->successors.remove(this);
  }

  /// Removes this OpDependencyNode from the graph by replacing all successor
  /// dependencies with the relevant dependency from this node. Also deletes
  /// this node and any classical values that only this node depends on.
  ///
  /// `erase` will not handle classical successors of this operation
  /// (e.g., a `quake.discriminate` if this operation is a `quake.mz`, or any
  /// classical results of an `if`). It is the responsibility of the caller
  /// to cleanup such values. Similarly, it is up to the caller to delete this
  /// node after it is erased.
  void erase() {
    for (auto successor : successors) {
      bool remove = true;
      for (auto &edge : successor->dependencies) {
        if (edge.node == this) {
          // If the output isn't a linear type, then don't worry about it
          if (quake::isQuantumType(edge.getValue().getType())) {
            auto idx = getDependencyForQID(edge.qid.value()).value();
            auto dependency = dependencies[idx];
            edge = dependency;
            dependency->successors.insert(successor);
          } else {
            remove = false;
          }
        }
      }

      if (remove) {
        successors.remove(successor);
        successor->updateHeight();
      }
    }

    // Clean up any now unused constants this node relies on
    for (auto dependency : dependencies) {
      dependency->successors.remove(this);
      if (dependency->successors.empty() && !dependency->isQuantumDependent()) {
        // TODO: probably not necessary to call erase here, as the dependency
        // now has no successors, and should be a classical constant, so
        // wouldn't have any dependencies either
        static_cast<OpDependencyNode *>(dependency.node)->erase();
        delete dependency.node;
      }
    }
  }

  virtual std::optional<VirtualQID> getQIDForResult(std::size_t resultidx) override {
    if (!isQuantumOp())
      return std::nullopt;
    auto operand = getOperandIDXFromResultIDX(resultidx, associated);
    if (operand >= dependencies.size())
      return std::nullopt;
    return dependencies[operand].qid;
  }
};

/// A DependencyGraph is a DAG consisting of DependencyNodes somehow related by
/// interaction. It contains useful metadata and functions for reasoning about
/// and manipulating the DAG.
class DependencyGraph {
private:
  // The set of root nodes in the DAG
  // TODO: why is this a Set and `leafs` a Map? I don't really remember
  SetVector<DependencyNode *> roots;
  // Tracks the node for the alloc of each virtual wire allocated in the DAG
  DenseMap<VirtualQID, InitDependencyNode *> allocs;
  // Tracks the leaf node for each virtual wire in the DAG
  DenseMap<VirtualQID, DependencyNode *> leafs;
  // The set of virtual wires used in the DAG. Each such virtual wire should
  // have a related leaf and root.
  SetVector<VirtualQID> qids;
  // Tracks the dependency node introducing each physical qubit in the DAG.
  // Currently, since physical qubit allocations are always lifted, the
  // associated DependencyNode will always be an InitDependencyNode. However, if
  // they were not always lifted, than it may also be a container DependencyNode
  // somewhere inside of which the qubit is allocated.
  // TODO: if physical wires are not combined, this needs to be a set, as the
  // same physical qubit can be allocated, used, and de-allocated multiple times
  // in a graph, which would present problems.
  DenseMap<PhysicalQID, DependencyNode *> qubits;
  unsigned total_height;
  SetVector<DependencyNode *> containers;

  /// Starting from \p next, searches through \p next's family
  /// (excluding already seen nodes) to find all the interconnected roots
  /// that this graph represents.
  /// Also fills in metadata about the height of the graph, and the qids in the
  /// graph.
  void gatherRoots(SetVector<DependencyNode *> &seen, DependencyNode *next) {
    if (seen.contains(next) || !next->isQuantumDependent())
      return;

    if (next->isRoot()) {
      roots.insert(next);
      if (next->height > total_height)
        total_height = next->height;
    }

    seen.insert(next);

    if (next->isLeaf() && next->isQuantumOp()) {
      leafs.insert({next->qids.front(), next});
      qids.insert(next->qids.front());
    }

    if (next->isAlloc()) {
      auto init = static_cast<InitDependencyNode *>(next);
      allocs[init->getQID()] = init;
    }

    if (next->isContainer())
      containers.insert(next);

    for (auto successor : next->successors)
      gatherRoots(seen, successor);
    for (auto dependency : next->dependencies)
      gatherRoots(seen, dependency.node);
  }

  /// Recursively finds all nodes in the graph scheduled at \p cycle
  SetVector<DependencyNode *> getNodesAtCycle(unsigned cycle) {
    SetVector<DependencyNode *> nodes;
    SetVector<DependencyNode *> seen;
    for (auto root : roots)
      nodes.set_union(root->getNodesAtCycle(cycle, seen));
    return nodes;
  }

  /// Recursively updates the height metadata of dependencies of \p next, and
  /// then \p next itself, skipping nodes in \p seen. Every updated node is
  /// added to \p seen. Dependencies are updated first so that the update to \p
  /// next uses up-to-date information.
  void updateHeight(SetVector<DependencyNode *> &seen, DependencyNode *next) {
    if (seen.contains(next))
      return;

    seen.insert(next);

    for (auto dependency : next->dependencies)
      updateHeight(seen, dependency.node);

    next->updateHeight();
  }

  /// Assigns cycles to quantum operations. A node must be scheduled after all
  /// of its dependencies, and before all of its successors. A node cannot be
  /// scheduled at a negative cycle, nor can it be scheduled at a cycle greater
  /// than or equal to the height of the graph to which it belongs.
  ///
  /// The scheduling algorithm (as currently implemented) works by always
  /// following the longest path first.
  /// The longest path will always be "saturated" with an operation every cycle,
  /// so we know exactly when to schedule every operation along that path.
  /// Then, every successor (not on the path) of an operation on the path should
  /// be scheduled as early as possible, (the earliest an operation can be
  /// scheduled is determined by its height). Likewise, every dependency (not on
  /// the path) should be scheduled as late as possible. Because we work
  /// outwards from the longest path, this ensures that every other path is
  /// scheduled as "densely" as possible around the connections with the longest
  /// path, while still having a valid schedule.
  ///
  /// Always following the longest path first is essentially an implementation
  /// of a transitive reduction of the graph. The only auxiliary data structure
  /// used here is a sorted copy of the dependency list. The length of a path
  /// is equal to the height of the node which is metadata present from
  /// construction.
  ///
  /// \p level is essentially the depth from the tallest point in the graph
  void schedule(SetVector<DependencyNode *> &seen, DependencyNode *next,
                unsigned level) {
    // Ignore classical values that don't depend on quantum values
    if (seen.contains(next) || !next->isQuantumDependent())
      return;

    seen.insert(next);

    // The height of a node (minus numTicks()) is the earliest a node can be
    // scheduled
    if (level < next->height)
      level = next->height;

    unsigned current = level;
    if (!next->isSkip()) {
      current -= next->numTicks();
      next->cycle = current;
    }

    // Sort dependencies by height to always follow the longest path first.
    // Without this, two dependencies may be scheduled at the same cycle,
    // even if one of the dependencies depends on the other.
    // This sort of mimics working over a transitive reduction of the graph.
    SmallVector<DependencyNode::DependencyEdge> sorted(next->dependencies);
    std::sort(sorted.begin(), sorted.end(), [](auto x, auto y) {
      return x.node->getHeight() > y.node->getHeight();
    });

    // Schedule dependencies as late as possible
    for (auto dependency : sorted)
      if (!dependency->isLeaf())
        schedule(seen, dependency.node, current);

    // Schedule unscheduled successors as early as possible
    for (auto successor : next->successors)
      if (!successor->isRoot())
        schedule(seen, successor,
                 current + next->numTicks() + successor->numTicks());
  }

  /// Replaces the leaf for \p old_qid (if \p old_qid is part of the graph) with
  /// \p new_leaf which has \p new_qid by removing the old leaf for \p old_qid
  /// from the graph metadata and replacing the dependency on the old leaf with
  /// \p new_leaf.
  ///
  /// If \p old_qid was not part of the graph, this has the effect of adding \p
  /// new_leaf to the graph.
  ///
  /// Cleaning up the old leaf is the responsibility of the caller.
  // TODO: replaceLeaf, replaceRoot, and replaceLeafAndRoot have confusing and
  //       overlapping functionality and are used to both replace and add
  //       leafs/roots. This makes them quite fragile. These responsibilities
  //       should be clearly separated into `add` and `replace` functions.
  //       Specifically, replaceLeaf can be used to both add a new leaf (but
  //       then old_qid is still passed with meaning which is bizarre), but also
  //       can be used to actually replace a leaf with a new leaf, as intended.
  //       There should really be a separate mechanism to add a new virtual wire
  //       to a graph along with a corresponding leaf and root (used when
  //       lifting allocations), and then a separate mechanism like here to get
  //       rid of the old leaf, and update the metadata (used for lowering
  //       allocations, to replace the block argument and terminator dependency
  //       with an alloc and de-alloc respectively). This change would
  //       additionally benefit from the TODO to make qids properly globally
  //       unique.
  void replaceLeaf(VirtualQID old_qid, VirtualQID new_qid,
                   DependencyNode *new_leaf) {
    assert(new_leaf->isLeaf() && "Invalid leaf!");

    if (leafs.count(old_qid) == 1) {
      auto old_leaf = leafs[old_qid];

      auto first_use = old_leaf->getSuccessorForQID(old_qid);
      auto idx = first_use->getDependencyForQID(old_qid).value();

      first_use->dependencies[idx] =
          DependencyNode::DependencyEdge(new_leaf, 0);
      old_leaf->successors.remove(first_use);
      new_leaf->successors.insert(first_use);
      if (old_leaf->isAlloc()) {
        allocs.erase(allocs.find(old_qid));
        auto alloc = static_cast<InitDependencyNode *>(old_leaf);
        if (alloc->qubit)
          qubits.erase(qubits.find(alloc->qubit.value()));
      }
    }

    leafs[new_qid] = new_leaf;
    if (new_leaf->isAlloc()) {
      auto alloc = static_cast<InitDependencyNode *>(new_leaf);
      allocs[new_qid] = alloc;
      if (alloc->qubit)
        qubits[alloc->qubit.value()] = alloc;
    }
  }

  /// Replaces the root for \p old_qid (if \p old_qid is part of the graph) with
  /// \p new_root which has \p new_qid by removing the old root for \p old_qid
  /// from the graph metadata and replacing the old root with \p new_root as the
  /// successor of the last use of \p old_qid.
  ///
  /// If \p old_qid was not part of the graph, this has the effect of adding \p
  /// new_root to the graph.
  ///
  /// Cleaning up the old root is the responsibility of the caller.
  // TODO: same thing as replaceLeaf above
  void replaceRoot(VirtualQID old_qid, VirtualQID new_qid,
                   DependencyNode *new_root) {
    assert(new_root->isRoot() && "Invalid root!");

    if (qids.contains(old_qid)) {
      auto old_root = getRootForQID(old_qid);

      auto idx = old_root->getDependencyForQID(old_qid).value();

      auto dep = old_root->dependencies[idx];
      dep->successors.remove(old_root);
      dep->successors.insert(new_root);

      new_root->dependencies.push_back(dep);
      old_root->dependencies.erase(old_root->dependencies.begin() + idx);

      // If the terminator is somehow getting deleted, then the entire block
      // must be empty, and then it will never be used
      if (old_root->dependencies.size() == 0)
        roots.remove(old_root);

      old_root->qids.remove(old_qid);
    }

    new_root->qids.insert(new_qid);
    // new_root->qids.insert(old_qid);
    roots.insert(new_root);
  }

  /// Gathers all the nodes in the graph into seen, starting from next
  void gatherNodes(SetVector<DependencyNode *> &seen, DependencyNode *next) {
    if (seen.contains(next) || !next->isQuantumDependent())
      return;

    seen.insert(next);

    for (auto successor : next->successors)
      gatherNodes(seen, successor);
    for (auto dependency : next->dependencies)
      gatherNodes(seen, dependency.node);
  }

public:
  DependencyGraph(DependencyNode *root) {
    total_height = 0;
    SetVector<DependencyNode *> seen;
    qids = SetVector<VirtualQID>();
    gatherRoots(seen, root);
  }

  /// Cleans up all nodes in the graph, except for ArgDependencyNodes, which are
  /// the responsibility of the DependencyBlock that owns this graph
  ~DependencyGraph() {
    SetVector<DependencyNode *> nodes;
    for (auto root : roots)
      gatherNodes(nodes, root);

    for (auto node : nodes)
      // ArgDependencyNodes are handled by the block and skipped here.
      // ShadowDependencyNodes are deleted here. This is safe, because
      // a new ShadowDependencyNode is created for each use of a
      // ShadowDependency (which may be undesirable eventually).
      if (!node->isLeaf() || !node->isQuantumDependent() || node->isAlloc())
        delete node;
  }

  /// Returns a set of all roots in the DAG
  SetVector<DependencyNode *> &getRoots() { return roots; }

  /// Calculates the LifeTime for the virtual wire \p qid.
  /// The graph must be scheduled, and the wire must be used in at least one
  /// operation for this function to succeed.
  LifeTime getLifeTimeForQID(VirtualQID qid) {
    auto first_use = getFirstUseOfQID(qid);
    assert(first_use && "Cannot compute LifeTime of unused qid");
    auto last_use = getLastUseOfQID(qid);
    assert(last_use && "Cannot compute LifeTime of unused qid");
    assert(first_use->cycle.has_value() &&
           "Graph must be scheduled before lifetimes can be ascertained");
    assert(last_use->cycle.has_value() &&
           "Graph must be scheduled before lifetimes can be ascertained");
    unsigned first = first_use->cycle.value();
    auto last = last_use->cycle.value();

    return LifeTime(first, last);
  }

  /// Calculates the LifeTime for \p qubit.
  /// The graph must be scheduled, and \p qubit must be used in at least one
  /// operation for this function to succeed.
  LifeTime getLifeTimeForQubit(PhysicalQID qubit) {
    DependencyNode *first_use = getFirstUseOfQubit(qubit);
    assert(first_use && "Cannot compute LifeTime of unused qubit");
    DependencyNode *last_use = getLastUseOfQubit(qubit);
    assert(last_use && "Cannot compute LifeTime of unused qubit");

    assert(first_use->cycle.has_value() &&
           "Graph must be scheduled before lifetimes can be ascertained");
    assert(last_use->cycle.has_value() &&
           "Graph must be scheduled before lifetimes can be ascertained");
    unsigned first = first_use->cycle.value();
    auto last = last_use->cycle.value();

    return LifeTime(first, last);
  }

  /// Returns the first use of the virtual wire \p qid, or nullptr if \p qid is
  /// unused in the graph.
  // TODO: could make this a little safer by having a separate "hasUse" check,
  // and then asserting that here.
  OpDependencyNode *getFirstUseOfQID(VirtualQID qid) {
    assert(qids.contains(qid) && "Given qid not in dependency graph");
    DependencyNode *firstUse = leafs[qid]->successors[0];
    if (firstUse->isRoot())
      return nullptr;
    // If a node is neither a root or leaf, it must be an OpDependencyNode
    return static_cast<OpDependencyNode *>(firstUse);
  }

  /// Returns the last use of the virtual wire \p qid, or nullptr if \p qid is
  /// unused in the graph.
  // TODO: could make this a little safer by having a separate "hasUse" check,
  // and then asserting that here.
  OpDependencyNode *getLastUseOfQID(VirtualQID qid) {
    assert(qids.contains(qid) && "Given qid not in dependency graph");
    DependencyNode *root = getRootForQID(qid);
    DependencyNode *lastUse = nullptr;
    for (auto dependency : root->dependencies) {
      if (dependency.qid == qid) {
        lastUse = dependency.node;
        break;
      }
    }
    if (lastUse && lastUse->isLeaf())
      return nullptr;
    // If a node is neither a root or leaf, it must be an OpDependencyNode
    return static_cast<OpDependencyNode *>(lastUse);
  }

  /// Returns the first use of the physical qubit \p qubit, or nullptr if \p
  /// qubit is unused in the graph.
  // TODO: could make this a little safer by having a separate "hasUse" check,
  // and then asserting that here.
  OpDependencyNode *getFirstUseOfQubit(PhysicalQID qubit) {
    assert((qubits.count(qubit) == 1) && "Given qubit not in dependency graph");
    auto defining = qubits[qubit];
    // Qubit is defined here, return the first use
    if (defining->isAlloc()) {
      auto first_use = defining->successors.front();
      if (first_use->isRoot())
        return nullptr;
      return static_cast<OpDependencyNode *>(first_use);
    }

    // Qubit is defined in a container which is an OpDependencyNode
    return static_cast<OpDependencyNode *>(defining);
  }

  /// Returns the last use of the physical qubit \p qubit, or nullptr if \p
  /// qubit is unused in the graph.
  // TODO: could make this a little safer by having a separate "hasUse" check,
  // and then asserting that here.
  OpDependencyNode *getLastUseOfQubit(PhysicalQID qubit) {
    assert((qubits.count(qubit) == 1) && "Given qubit not in dependency graph");
    auto defining = qubits[qubit];
    // Qubit is defined here, return the last use
    if (defining->isAlloc()) {
      auto alloc = static_cast<InitDependencyNode *>(defining);
      return getLastUseOfQID(alloc->getQID());
    }

    // Qubit is defined in a container which is an OpDependencyNode
    return static_cast<OpDependencyNode *>(defining);
  }

  /// Returns the alloc node for the virtual wire \p qid, fails if no such node
  /// is found
  InitDependencyNode *getAllocForQID(VirtualQID qid) {
    assert(allocs.count(qid) == 1 && "Given qid not allocated in graph");
    return allocs[qid];
  }

  /// Returns the root for the virtual wire \p qid, fails if no such root is
  /// found
  DependencyNode *getRootForQID(VirtualQID qid) {
    assert(qids.contains(qid) && "Given qid not in dependency graph");
    for (auto root : roots)
      if (root->qids.contains(qid))
        return root;

    assert(false && "Could not find root for qid");
  }

  /// Returns the alloc node for the physical qubit \p qubit, fails if no such
  /// node is found
  InitDependencyNode *getAllocForQubit(PhysicalQID qubit) {
    assert(qubits.count(qubit) == 1 && qubits[qubit]->isAlloc() &&
           "Given qubit not allocated in graph!");
    return static_cast<InitDependencyNode *>(qubits[qubit]);
  }

  /// Returns the root for the physical qubit \p qubit, fails if no such root is
  /// found
  DependencyNode *getRootForQubit(PhysicalQID qubit) {
    for (auto root : roots)
      if (root->getQubits().contains(qubit))
        return root;
    assert(false && "Could not find root for qubit");
  }

  /// Generate code for all nodes at the given cycle in the graph,
  /// as well as all non-quantum nodes relying on those nodes with
  /// no other dependencies at later cycles.
  void codeGenAt(unsigned cycle, OpBuilder &builder, LifeTimeAnalysis &set) {
    SetVector<DependencyNode *> nodes = getNodesAtCycle(cycle);

    for (auto node : nodes)
      node->codeGen(builder, set);
  }

  unsigned getHeight() { return total_height; }

  /// Returns a set containing all virtual wires used in this DAG
  SetVector<VirtualQID> getQIDs() { return SetVector<VirtualQID>(qids); }

  /// Returns the set of virtual wires allocated in the DAG
  SetVector<VirtualQID> getVirtualAllocs() {
    SetVector<VirtualQID> allocated;
    for (auto [qid, leaf] : allocs)
      if (!leaf->getQubit())
        allocated.insert(qid);
    return allocated;
  }

  /// Returns the set of all physical qubits in the DAG
  SetVector<PhysicalQID> getQubits() {
    auto allocated = SetVector<PhysicalQID>();
    for (auto [qubit, _] : qubits)
      allocated.insert(qubit);
    return allocated;
  }

  /// Returns the set of physical qubits allocated in the DAG
  SetVector<PhysicalQID> getAllocatedQubits() {
    auto allocated = SetVector<PhysicalQID>();
    for (auto [qubit, definining] : qubits)
      if (definining->isAlloc())
        allocated.insert(qubit);
    return allocated;
  }

  /// Assigns the virtual wire \p qid to the physical qubit \p phys,
  /// assuming that \p qid is allocated in the graph.
  void assignToPhysical(VirtualQID qid, PhysicalQID phys) {
    // Call helper function to perform relevant checks
    auto alloc = getAllocForQID(qid);
    qubits[phys] = alloc;
    alloc->assignToPhysical(phys);
  }

  /// If a physical wire representing \p phys exists, combines the virtual wire
  /// \p qid with the physical wire representing \p phys, resulting in a single
  /// physical wire \p phys. Otherwise, works like `assignToPhysical`.
  ///
  /// If combining with an existing physical wire, this function will clean up
  /// the extra allocation/de-allocation nodes for the physical wire after
  /// combining.
  void combineWithPhysicalWire(VirtualQID qid, PhysicalQID phys) {
    if (qubits.count(phys) != 1) {
      assignToPhysical(qid, phys);
      return;
    }

    assert(allocs.count(qid) == 1 && "Assigning a qid not in DependencyGraph!");
    auto new_lifetime = getLifeTimeForQID(qid);
    auto old_lifetime = getLifeTimeForQubit(phys);

    // TODO: can probably clean up a bit
    if (new_lifetime.isAfter(old_lifetime)) {
      auto new_alloc = getAllocForQID(qid);
      auto old_root = getRootForQubit(phys);

      auto successor = new_alloc->getSuccessorForQID(qid);
      auto idx = successor->getDependencyForQID(qid).value();

      // Replace new allocation with result value for old wire
      auto dep = old_root->dependencies[0];
      successor->dependencies[idx] = dep;
      dep->successors.insert(successor);
      dep->successors.remove(old_root);

      dep->updateQID(new_alloc->getQID(), dep.qid.value());

      roots.remove(old_root);
      delete old_root;
      allocs.erase(allocs.find(new_alloc->getQID()));
      delete new_alloc;

      successor->updateWithPhysical(dep.qid.value(), phys);
    } else {
      auto old_alloc = getAllocForQubit(phys);
      auto new_root = getRootForQID(qid);

      auto successor = old_alloc->getSuccessorForQID(old_alloc->getQID());
      auto idx = successor->getDependencyForQID(old_alloc->getQID()).value();

      auto dep = new_root->dependencies[0];
      successor->dependencies[idx] = dep;
      dep->successors.insert(successor);
      dep->successors.remove(new_root);

      dep->updateQID(old_alloc->getQID(), dep.qid.value());

      roots.remove(new_root);
      allocs.erase(allocs.find(old_alloc->getQID()));
      delete old_alloc;
      delete new_root;

      auto new_alloc = getAllocForQID(qid);
      new_alloc->assignToPhysical(phys);
      qubits[phys] = new_alloc;
    }
  }

  /// Tells the graph that
  void addPhysicalAllocation(DependencyNode *container, PhysicalQID qubit) {
    assert(containers.contains(container) &&
           "Illegal container in addPhysicalAllocation");
    qubits[qubit] = container;
  }

  /// Qubits allocated within a dependency block that are only used inside an
  /// `if` in that block, can be moved inside the `if`.
  ///
  /// Works outside-in, to contract as tightly as possible.
  void contractAllocsPass() {
    for (auto container : containers)
      container->contractAllocsPass();
  }

  /// Assigns a cycle to every quantum operation in each dependency graph
  /// (including `if`s containing quantum operations).
  ///
  /// Every node must be assigned a schedule greater than or equal to the height
  /// of each of its dependencies
  ///
  /// The current implementation of the scheduling algorithm can be found in
  /// DependencyGraph::schedule
  void schedulingPass() {
    SetVector<DependencyNode *> seen;
    // Schedule from the roots in order of height (starting from the tallest
    // root)
    auto sorted = SmallVector<DependencyNode *>({roots.begin(), roots.end()});
    std::sort(sorted.begin(), sorted.end(),
              [](auto x, auto y) { return x->getHeight() > y->getHeight(); });

    // Every node visiting during scheduling will be in seen, so
    // if the scheduling function has already visited the root then it will be
    // skipped
    for (auto root : sorted) {
      // Can either schedule starting with a level of `root->getHeight()`, which
      // will result in more operations at earlier cycles, or `total_height`,
      // which will result in more operations at later cycles
      schedule(seen, root, total_height);
    }
  }

  void print() {
    llvm::outs() << "Graph Start\n";
    for (auto root : roots)
      root->print();
    llvm::outs() << "Graph End\n";
  }

  /// Recursively invokes performAnalysis on all container nodes within this DAG
  void performAnalysis(LifeTimeAnalysis &set) {
    for (auto container : containers)
      container->performAnalysis(set, this);
  }

  /// Removes the alloc/de-alloc nodes for \p qid, assuming \p qid is allocated
  /// within this DAG It is the responsibility of the caller to delete the nodes
  /// if desired.
  // TODO: ensure callers cleanup the nodes properly (it doesn't look like
  // contractallocsPass or lowerAlloc do)
  void removeVirtualAlloc(VirtualQID qid) {
    // TODO: This function does not look right. First, it should ensure that \p
    // qid is actually allocated in this graph, to avoid, among other issues,
    // removing the TerminatorDependencyNode from the graph.
    //       Second, it should remove both the alloc and the root together, not
    //       in separate checks. Third, I don't know about the below comment
    //       "ignore already removed qid", that should probably be an error if
    //       you're trying to remove a qid again. This is currently only used by
    //       contractAllocsPass, so probably was written overly specific for
    //       that use case.
    // Ignore already removed qid
    if (allocs.count(qid) == 1)
      allocs.erase(allocs.find(qid));

    if (qids.contains(qid)) {
      auto toRemove = getRootForQID(qid);
      roots.remove(toRemove);
    }
  }

  /// Simultaneously replaces the leaf and root nodes for a given qid, or
  /// adds them if the qid was not present before. The operations are separate,
  /// but doing them together makes it harder to produce an invalid graph.
  ///
  /// It is the responsibility of the caller to delete the replaced leaf/root if
  /// desired.
  // TODO: See above comment on `replaceLeaf`: this function is pretty fragile
  // as currently written and used.
  // TODO: Worth checking that callers delete the replaced leaf/root properly
  // when applicable. I think lowerAlloc does, which is the main place.
  void replaceLeafAndRoot(VirtualQID qid, DependencyNode *new_leaf,
                          DependencyNode *new_root) {
    auto new_qid = qid;
    if (!new_leaf->qids.empty())
      new_qid = new_leaf->qids.front();

    replaceLeaf(qid, new_qid, new_leaf);
    replaceRoot(qid, new_qid, new_root);

    qids.insert(new_qid);

    if (new_qid != qid) {
      qids.remove(qid);
      new_leaf->updateQID(qid, new_qid);
    }
  }

  /// Removes QID from the metadata for this graph
  void removeQID(VirtualQID qid) {
    leafs.erase(leafs.find(qid));
    qids.remove(qid);
  }

  void updateHeight() {
    total_height = 0;
    SetVector<DependencyNode *> seen;
    for (auto root : roots) {
      updateHeight(seen, root);
      if (root->height > total_height)
        total_height = root->height;
    }
  }
};

/// Represent the deallocation (`quake.return_wire` op) of a virtual/physical
/// wire
class RootDependencyNode : public OpDependencyNode {
protected:
  void printNode() override {
    llvm::outs() << "Dealloc for QID ";
    for (auto qid : qids)
      llvm::outs() << qid;
    llvm::outs() << ": ";
    associated->dump();
  }

  void genOp(OpBuilder &builder, LifeTimeAnalysis &set) override {
    auto wire = dependencies[0].getValue();
    auto newOp =
        builder.create<quake::ReturnWireOp>(builder.getUnknownLoc(), wire);
    newOp->setAttrs(associated->getAttrs());
    newOp->removeAttr("dnodeid");
    associated = newOp;
  }

public:
  RootDependencyNode(quake::ReturnWireOp op,
                     SmallVector<DependencyEdge> dependencies)
      : OpDependencyNode(op, dependencies) {
    // TODO: does this below comment still hold?
    // numTicks won't be properly calculated by OpDependencyNode constructor,
    // so have to recompute height here
    updateHeight();
  };

  ~RootDependencyNode() override {}

  bool isSkip() override { return true; }

  unsigned numTicks() override { return 0; }

  void eraseEdgeForQID(VirtualQID qid) override {
    if (qids.contains(qid))
      dependencies.clear();
  }

  SetVector<PhysicalQID> getQubits() override {
    SetVector<PhysicalQID> qubits;
    for (auto dependency : dependencies)
      if (dependency.qubit.has_value())
        qubits.insert(dependency.qubit.value());
    return qubits;
  }
};

/// Represents a block argument. Block arguments must have linear types,
/// and therefore will always represent wires.
class ArgDependencyNode : public DependencyNode {
  friend class DependencyBlock;
  friend class IfDependencyNode;

protected:
  BlockArgument barg;
  unsigned argNum;

  void printNode() override {
    if (qids.size() > 0)
      llvm::outs() << "QID: " << qids.front() << ", ";
    barg.dump();
  }

  Value getResult(unsigned resultidx) override {
    assert(resultidx == 0 && "Invalid resultidx");
    return barg;
  }

  virtual std::string getOpName() override {
    return std::to_string(barg.getArgNumber()).append("arg");
  };

  void codeGen(OpBuilder &builder, LifeTimeAnalysis &set) override{};

public:
  ArgDependencyNode(BlockArgument arg)
      : barg(arg), argNum(arg.getArgNumber()) {}

  ArgDependencyNode(BlockArgument arg, unsigned num) : barg(arg), argNum(num) {}

  ArgDependencyNode(BlockArgument arg, DependencyEdge val)
      : ArgDependencyNode(arg) {
    auto qid = val->getQIDForResult(val.resultidx);
    if (qid.has_value())
      qids.insert(qid.value());
  }

  ArgDependencyNode(BlockArgument arg, DependencyEdge val, unsigned num)
      : barg(arg), argNum(num) {
    auto qid = val->getQIDForResult(val.resultidx);
    if (qid.has_value())
      qids.insert(qid.value());
  }

  ~ArgDependencyNode() override {}

  bool isRoot() override { return false; }
  bool isLeaf() override { return true; }
  // TODO: I'm pretty sure this is always true
  bool isQuantumOp() override { return quake::isQuantumType(barg.getType()); }
  unsigned numTicks() override { return 0; }

  void eraseEdgeForQID(VirtualQID qid) override {
    assert(false && "Can't call eraseEdgeForQID with an ArgDependencyNode");
  }

  std::optional<VirtualQID> getQIDForResult(std::size_t resultidx) override {
    assert(resultidx == 0 && "Invalid resultidx");
    if (qids.size() == 1)
      return std::optional(qids.front());
    return std::nullopt;
  }

  unsigned getArgNumber() { return argNum; }
};

/// Wires are linear types and therefore are passed as operands to `if`s,
/// ensuring that `if`s act properly as "solid containers" for them. However,
/// this is not the case with quantum-dependent classical values. To ensure the
/// solidity of `if`s, we introduce "shadow dependencies" between `if`s and any
/// quantum-dependent classical values used within the body of the `if`. Then,
/// instead of referring to the value directly within the `if`, we use a
/// ShadowDependencyNode to depend on the value without depending on the node
/// for the value since the node for the value is located in a different graph.
///
/// A concrete example of where things can go wrong without shadow dependencies
/// is in `test/Quake/dependency-if-bug-classical.qke`.
class ShadowDependencyNode : public DependencyNode {
  friend class DependencyBlock;
  friend class IfDependencyNode;

protected:
  OpDependencyNode *shadowed;
  DependencyEdge shadow_edge;

  void printNode() override {
    llvm::outs() << "Shadow dependency on: ";
    shadowed->printNode();
  }

  Value getResult(unsigned resultidx) override {
    return shadowed->getResult(resultidx);
  }

  virtual std::string getOpName() override {
    return shadowed->getOpName().append("shadow");
  };

  void codeGen(OpBuilder &builder, LifeTimeAnalysis &set) override {
    // Don't generate any code, instead just ensure that the
    if (shadowed->hasCodeGen)
      hasCodeGen = true;
  };

public:
  // TODO: constructor should ensure that the value from shadowed is not a
  // quantum type (but that shadowed is quantumDependent).
  ShadowDependencyNode(OpDependencyNode *shadowed, std::size_t resultidx)
      : shadowed(shadowed), shadow_edge(shadowed, resultidx) {}

  ~ShadowDependencyNode() override {}

  bool isRoot() override { return false; }
  bool isLeaf() override { return true; }
  bool isQuantumOp() override { return false; }
  unsigned numTicks() override { return 0; }

  void eraseEdgeForQID(VirtualQID qid) override {
    assert(false && "Can't call eraseEdgeForQID with an ShadowDependencyNode");
  }

  std::optional<VirtualQID> getQIDForResult(std::size_t resultidx) override {
    return std::nullopt;
  }

  DependencyEdge getShadowedEdge() { return shadow_edge; }
};

/// Represents a block terminator, usually either a `cc.continue` or a `return`.
/// Importantly, a block terminator should only have code generated for it
/// after all over nodes in the graph have code generated, so that it is always
/// the last operation in the block.
class TerminatorDependencyNode : public OpDependencyNode {
protected:
  void printNode() override {
    llvm::outs() << "Block Terminator With QIDs ";
    bool printComma = false;
    for (auto qid : qids) {
      if (printComma)
        llvm::outs() << ", ";
      llvm::outs() << qid;
      printComma = true;
    }
    llvm::outs() << ": ";
    associated->dump();
  }

  unsigned numTicks() override { return 0; }

  bool isQuantumOp() override { return qids.size() > 0; }

  // If the terminator is not a quantum operation, this could be called
  // by dependencies, so do nothing.
  void codeGen(OpBuilder &builder, LifeTimeAnalysis &set) override{};

public:
  TerminatorDependencyNode(Operation *terminator,
                           SmallVector<DependencyEdge> dependencies)
      : OpDependencyNode(terminator, dependencies) {
    assert(terminator->hasTrait<mlir::OpTrait::ReturnLike>() &&
           "Invalid terminator");
    for (auto dependency : dependencies)
      if (dependency.qid.has_value())
        qids.insert(dependency.qid.value());
  }

  ~TerminatorDependencyNode() override {}

  /// This will actually generate code for the terminator, it should only be
  /// called after all other operations in the block have code generated.
  void genTerminator(OpBuilder &builder, LifeTimeAnalysis &set) {
    OpDependencyNode::codeGen(builder, set);
  }

  void eraseEdgeForQID(VirtualQID qid) override {
    for (unsigned i = 0; i < dependencies.size(); i++)
      if (dependencies[i].qid == qid)
        dependencies.erase(dependencies.begin() + i);
  }

  std::optional<VirtualQID> getQIDForResult(std::size_t resultidx) override {
    if (resultidx >= dependencies.size() ||
        !dependencies[resultidx]->isQuantumOp())
      return std::nullopt;
    return dependencies[resultidx].qid;
  }

  SetVector<PhysicalQID> getQubits() override {
    SetVector<PhysicalQID> qubits;
    for (auto dependency : dependencies)
      if (dependency.qubit.has_value())
        qubits.insert(dependency.qubit.value());
    return qubits;
  }
};

/// A DependencyBlock represents an mlir::block.
/// It contains a DependencyGraph representing the block body,
/// ArgDependencyNodes for the block arguments, and a TerminatorDependencyNode
/// for the block terminator.
class DependencyBlock {
private:
  SmallVector<ArgDependencyNode *> argdnodes;
  DependencyGraph *graph;
  Block *block;
  TerminatorDependencyNode *terminator;

public:
  DependencyBlock(SmallVector<ArgDependencyNode *> argdnodes,
                  DependencyGraph *graph, Block *block,
                  TerminatorDependencyNode *terminator)
      : argdnodes(argdnodes), graph(graph), block(block),
        terminator(terminator) {}

  ~DependencyBlock() {
    // Terminator is cleaned up by graph since it must be a root
    delete graph;
    // Arguments are not handled by the graph since they may not show up in the
    // graph
    for (auto argdnode : argdnodes)
      delete argdnode;
  }

  unsigned getHeight() { return graph->getHeight(); }

  SetVector<VirtualQID> getVirtualAllocs() { return graph->getVirtualAllocs(); }

  SetVector<VirtualQID> getQIDs() { return graph->getQIDs(); }

  DependencyGraph *getBlockGraph() { return graph; }

  TerminatorDependencyNode *getTerminator() { return terminator; }

  /// Allocates physical qubits for all virtual wires
  /// allocated within the block, using lifetime information
  /// from the DependencyGraph representing the body.
  void allocatePhyiscalQubits(LifeTimeAnalysis &set) {
    for (auto qubit : graph->getQubits()) {
      auto lifetime = graph->getLifeTimeForQubit(qubit);
      set.reallocatePhysical(qubit, lifetime);
    }

    // New physical qubits will be captured by `set`
    for (auto qid : getVirtualAllocs()) {
      if (!graph->getFirstUseOfQID(qid))
        continue;

      auto lifetime = graph->getLifeTimeForQID(qid);
      LLVM_DEBUG(llvm::dbgs() << "Qid " << qid);
      LLVM_DEBUG(llvm::dbgs()
                 << " is in use from cycle " << lifetime.getBegin());
      LLVM_DEBUG(llvm::dbgs() << " through cycle " << lifetime.getEnd());
      LLVM_DEBUG(llvm::dbgs() << "\n");

      auto phys = set.allocatePhysical(qid, lifetime);
      LLVM_DEBUG(llvm::dbgs()
                 << "\tIt is mapped to the physical qubit " << phys);
      LLVM_DEBUG(llvm::dbgs() << "\n\n");

      // This will assign the virtual wire qid to the physical wire phys,
      // combining with existing uses of phys to thread the wire through.
      // This ensures that further optimizations respect the schedule of
      // operations on phys, and that all qids mapped to phys remain mapped
      // to phys.

      // If this is not desired, use `graph->assignToPhysical` here instead,
      // but it is crucial to somehow otherwise ensure that the
      // scheduling of operations on phys is respected by further
      // optimizations, as there is the potential for incorrect IR output.

      // Also importantly, doing so will require changes to graph metadata,
      // to ensure that multiple allocations of the same physical wire within a
      // single graph are handled properly.
      graph->combineWithPhysicalWire(qid, phys);
    }
  }

  /// Generates code for the block arguments, body, and terminator.
  ///
  /// It is up to the caller to move the insertion point of \p builder outside
  /// the block after construction.
  Block *codeGen(OpBuilder &builder, Region *region, LifeTimeAnalysis &set) {
    Block *newBlock = builder.createBlock(region);
    for (unsigned i = 0; i < argdnodes.size(); i++) {
      auto old_barg = argdnodes[i]->barg;
      argdnodes[i]->barg =
          newBlock->addArgument(old_barg.getType(), old_barg.getLoc());
      assert(argdnodes[i]->barg.getArgNumber() == argdnodes[i]->argNum);
      argdnodes[i]->hasCodeGen = true;
    }

    builder.setInsertionPointToStart(newBlock);

    for (unsigned cycle = 0; cycle < graph->getHeight(); cycle++)
      graph->codeGenAt(cycle, builder, set);

    terminator->genTerminator(builder, set);

    block = newBlock;

    return newBlock;
  }

  void print() {
    llvm::outs() << "Block with (" << argdnodes.size() << ") args:\n";
    // block->dump();
    // llvm::outs() << "Block graph:\n";
    graph->print();
    llvm::outs() << "End block\n";
  }

  void updateHeight() { graph->updateHeight(); }

  /// Recursively schedules nodes and performs lifetime analysis to allocate
  /// physical qubits for virtual wires, working inside out. For
  /// `DependencyBlock`s, this means recurring on any containers inside
  /// the body of the block, then performing scheduling, and finally
  /// allocating physical qubits based on lifetime information.
  void performAnalysis(LifeTimeAnalysis &set) {
    // The analysis works inside-out, so first resolve all nested `if`s
    graph->performAnalysis(set);

    // Update metadata after the analysis
    updateHeight();
    // Schedule the nodes for lifetime analysis
    schedulingPass();

    // Finally, perform lifetime analysis and allocate physical qubits
    // Allocations will be captured in `set`
    allocatePhyiscalQubits(set);
  }

  /// Checks to see if qubits allocated within a block are only used
  /// inside an `if` in that block, in which case they can be moved
  /// inside the `if`.
  ///
  /// Works outside-in, to contract as tightly as possible.
  // TODO: pass unique counter to use to generate unique qids when
  //       splitting wires per inner block, see
  //       targettests/execution/qubit_management_bug_qids.cpp for
  //       an example of how the current approach fails.
  void contractAllocsPass() {
    // Look for contract-able allocations in this block
    for (auto alloc : getVirtualAllocs()) {
      auto first_use = graph->getFirstUseOfQID(alloc);
      assert(first_use && "Unused virtual wire in block!");
      auto last_use = graph->getLastUseOfQID(alloc);
      if (first_use == last_use && first_use->isContainer()) {
        // Move alloc inside
        auto root = graph->getRootForQID(alloc);
        auto init = graph->getAllocForQID(alloc);
        first_use->lowerAlloc(init, root, alloc);
        // Qid is no longer used in this block, remove related metadata
        graph->removeVirtualAlloc(alloc);
        graph->removeQID(alloc);
      }
    }

    // Outside-in, so recur only after applying pass to this block
    graph->contractAllocsPass();
  }

  /// Moves an alloc/de-alloc pair for the virtual wire \p qid into this block,
  /// Replacing the existing block argument and terminator dependencies for the
  /// wire.
  // TODO: should probably take a new qid in addition to the old qid, so that
  //       the qid can be changed when splitting so uniqueness of qids can be
  //       maintained
  void lowerAlloc(DependencyNode *init, DependencyNode *root, VirtualQID qid) {
    // No need to clean up existing terminator (hopefully)
    graph->replaceLeafAndRoot(qid, init, root);
    // Clean up old block argument
    removeArgument(qid);
    // If the qid isn't actually used in the block, remove it
    if (!graph->getFirstUseOfQID(qid)) {
      // TODO: clean up init and root in this case
      graph->removeVirtualAlloc(qid);
      graph->removeQID(qid);
    }
  }

  /// Removes an alloc/de-alloc pair for the virtual wire \p qid from this
  /// block, Replacing the pair with a new block argument and terminator
  /// dependency for the wire.
  ///
  /// The caller is responsible for cleaning up the old alloc/de-alloc pair.
  void liftAlloc(VirtualQID qid, DependencyNode *lifted_alloc) {
    auto new_edge = DependencyNode::DependencyEdge{lifted_alloc, 0};
    auto new_argdnode = addArgument(new_edge);

    graph->replaceLeafAndRoot(qid, new_argdnode, terminator);
  }

  void schedulingPass() { graph->schedulingPass(); }

  /// Removes a block argument/terminator dependency pair for a virtual wire \p
  /// qid flowing through this block
  void removeQID(VirtualQID qid) {
    // TODO: ensure that the virtual wire does flow through the block as an
    // argument/terminator pair removeArgument will at least ensure that such an
    // argument exists, but terminator->eraseEdgeForQID below won't.
    removeArgument(qid);

    terminator->eraseEdgeForQID(qid);
    graph->removeQID(qid);
  }

  SetVector<PhysicalQID> getQubits() { return graph->getQubits(); }

  SetVector<PhysicalQID> getAllocatedQubits() {
    return graph->getAllocatedQubits();
  }

  /// Adds a block argument and corresponding ArgDependencyNode to the block
  DependencyNode *addArgument(DependencyNode::DependencyEdge incoming) {
    auto new_barg = block->addArgument(incoming.getValue().getType(),
                                       incoming.getValue().getLoc());
    auto new_argdnode =
        new ArgDependencyNode(new_barg, incoming, argdnodes.size());
    argdnodes.push_back(new_argdnode);
    return new_argdnode;
  }

  /// Removes the block argument and cleans up the corresponding
  /// ArgDependencyNode for \p qid
  void removeArgument(VirtualQID qid) {
    for (unsigned i = 0; i < argdnodes.size(); i++)
      if (argdnodes[i]->qids.contains(qid)) {
        delete argdnodes[i];
        argdnodes.erase(argdnodes.begin() + i);
        return;
      }

    assert(false && "Could not find argument to remove!");
  }

  std::optional<VirtualQID> getQIDForResult(std::size_t resultidx) {
    return terminator->getQIDForResult(resultidx);
  }
};

class IfDependencyNode : public OpDependencyNode {
protected:
  DependencyBlock *then_block;
  DependencyBlock *else_block;
  SmallVector<Type> results;
  SetVector<DependencyNode *> freevars;

  // TODO: figure out nice way to display
  void printNode() override {
    this->OpDependencyNode::printNode();
    // llvm::outs() << "If with results:\n";
    // for (auto result : results)
    //   result.dump();
    llvm::outs() << "Then ";
    then_block->print();
    llvm::outs() << "Else ";
    else_block->print();
  }

  /// Checks if \p then_use and \p else_use are prefixEquivalent and have no
  /// quantum dependencies, and if so lifts them before the this `if` node.
  ///
  /// Assumes that \p then_use is from then_block and \p else_use is from
  /// else_block, but this is not checked.
  bool tryLiftingBefore(OpDependencyNode *then_use,
                        OpDependencyNode *else_use) {
    if (!then_use || !else_use)
      return false;

    // TODO: probably shouldn't try lifting containers
    // see targettests/execution/qubit_management_bug_lifting_ifs.cpp

    if (then_use->prefixEquivalentTo(else_use)) {
      // If two nodes are equivalent, all their dependencies will be too,
      // but we can't lift them until all their dependencies have been lifted,
      // so we skip them for now.
      if (then_use->height > then_use->numTicks())
        return false;

      liftOpBefore(then_use, else_use);
      return true;
    }

    return false;
  }

  /// Checks if \p then_use and \p else_use are equivalent and have no classical
  /// dependencies/results, and if so lifts them before the this `if` node.
  ///
  /// Assumes that \p then_use is from then_block and \p else_use is from
  /// else_block, but this is not checked.
  bool tryLiftingAfter(OpDependencyNode *then_use, OpDependencyNode *else_use) {
    // TODO: measure ops are a delicate special case because of the classical
    // measure result. When lifting before, we can lift the discriminate op as
    // well. However, it may have interactions with other classical values, and
    // then be "returned" from the `if`
    if (isa<RAW_MEASURE_OPS>(then_use->associated))
      return false;

    if (!then_use || !else_use)
      return false;

    // TODO: probably shouldn't try lifting containers
    // see targettests/execution/qubit_management_bug_lifting_ifs.cpp

    if (then_use->postfixEquivalentTo(else_use)) {
      // If two nodes are equivalent, all their successors should be too
      // but we can't lift them until all their successors have been lifted,
      // so we skip them for now.
      for (auto successor : then_use->successors)
        if (!successor->isSkip())
          return false;
      // TODO: Classical input from within the if scope poses an issue for
      // lifting for a similar reason as measures
      for (auto dependency : then_use->dependencies)
        if (!dependency->isQuantumOp())
          return false;

      liftOpAfter(then_use, else_use);
      return true;
    }

    return false;
  }

  /// Lifts equivalent operations from the then and else blocks to be added as a
  /// successor to this node. The lifted operation will have dependencies on the
  /// results from this `if` node. Wires that used to flow through
  /// then_op/else_op will still flow to the terminator and be returned.
  ///
  /// Assumes \p then_op and \p else_op are equivalent quantum operations from
  /// then_block and else_block respectively, without classical input or
  /// results.
  ///
  /// This function is responsible for cleaning up \p then_use and \p else_use
  /// after lifting.
  void liftOpAfter(OpDependencyNode *then_op, OpDependencyNode *else_op) {
    auto newDeps = SmallVector<DependencyEdge>();
    auto allocated = then_block->getAllocatedQubits();
    assert(then_op->dependencies.size() == then_op->successors.size());

    for (unsigned i = 0; i < then_op->dependencies.size(); i++) {
      auto dependency = then_op->dependencies[i];
      assert(dependency.qid && "Lifting operations with classical input after "
                               "blocks is not yet supported.");

      // TODO: if allocations are not always lifted, then it is necessarily to
      // lift allocations then_op depends on, but only if it is safe to lift.

      auto then_qid = dependency.qid.value();
      // Remove old edge from in this `if`
      then_op->eraseEdgeForQID(then_qid);

      // Add new edge from after this `if`
      auto resultidx = then_op->getResultForDependency(i);
      auto successor = getSuccessorForQID(then_qid);
      auto idx = successor->getDependencyForQID(then_qid).value();
      newDeps.push_back(successor->dependencies[idx]);
      successor->dependencies[idx] = DependencyEdge{then_op, resultidx};

      // Readd QID
      then_op->qids.insert(then_qid);
    }

    successors.insert(then_op);
    then_op->dependencies = newDeps;
    else_op->erase();
    delete else_op;
  }

  /// Lifts equivalent operations from the then and else blocks to be added as a
  /// dependency to this node. The lifted operation will have dependencies on
  /// block argument replaced with the relevant dependencies from this `if`
  /// node, and the relevant dependencies from this `if` node will be replaced
  /// with the results from the lifted operation.
  ///
  /// Assumes \p then_op and \p else_op are equivalent quantum operations from
  /// then_block and else_block respectively, without classical input or
  /// results.
  ///
  /// This function is responsible for cleaning up \p then_use and \p else_use,
  /// as well as any unused classical values depending on them after lifting.
  void liftOpBefore(OpDependencyNode *then_op, OpDependencyNode *else_op) {
    auto newDeps = SmallVector<DependencyEdge>();

    // Measure ops are a delicate special case because of the classical measure
    // result. When lifting before, we can lift the discriminate op as well,
    // but, the classical result is now free in the body of the if (assuming it
    // was used) so we must add a shadow dependency on it.
    // TODO: a similar problem can arise for classical results from lifted
    // `if`s.
    //       This will cause bugs currently. The easy solution is to avoid
    //       lifting `if`s, and the trickier solution is to add shadow
    //       dependencies for, and properly clean up, arbitrary classical
    //       results for lifted operations.
    if (isa<RAW_MEASURE_OPS>(then_op->associated)) {
      auto then_discriminate = then_op->successors.front()->isQuantumOp()
                                   ? then_op->successors.back()
                                   : then_op->successors.front();
      auto else_discriminate = else_op->successors.front()->isQuantumOp()
                                   ? else_op->successors.back()
                                   : else_op->successors.front();
      auto casted = static_cast<OpDependencyNode *>(then_discriminate);
      // Lifting the classical value requires adding a shadow dependency on it.
      // TODO: only do so if the classical value is used (and clean it up if
      // not).
      auto newfreevar = new ShadowDependencyNode(casted, 0);
      auto newEdge = DependencyEdge{newfreevar, 0};
      then_discriminate->replaceWith(newEdge);
      else_discriminate->replaceWith(newEdge);
      dependencies.push_back(newEdge);
      freevars.insert(newfreevar);

      delete else_discriminate;
    }

    // Construct new dependencies for then_op based on the dependencies for this
    // `if`
    for (unsigned i = 0; i < then_op->dependencies.size(); i++) {
      auto dependency = then_op->dependencies[i];

      if (freevars.contains(dependency.node)) {
        // If the dependency is a free variable with this `if` as the frontier,
        // then we can just use the value directly, instead of the shadowed
        // value
        auto shadowNode = static_cast<ShadowDependencyNode *>(dependency.node);
        auto edge = shadowNode->getShadowedEdge();
        newDeps.push_back(edge);
        shadowNode->successors.remove(then_op);
        // Remove shadowNode if it is no longer needed
        if (shadowNode->successors.empty()) {
          for (unsigned i = 0; i < dependencies.size(); i++)
            if (dependencies[i].node == edge.node &&
                dependencies[i].resultidx == edge.resultidx)
              dependencies.erase(dependencies.begin() + i);
          freevars.remove(shadowNode);
          delete shadowNode;
        }
      } else if (dependency->isLeaf() && dependency->isQuantumOp()) {
        // The dependency is a block argument, and therefore reflects a
        // dependency for this `if` First, find the relevant argument
        ArgDependencyNode *arg =
            static_cast<ArgDependencyNode *>(dependency.node);
        auto num = arg->getArgNumber();
        // Then, get the dependency from this `if` for the relevant argument,
        // this will be the new dependency for `then_op`
        auto newDep = dependencies[num + 1];
        newDep->successors.remove(this);
        newDep->successors.insert(then_op);
        newDeps.push_back(newDep);
        arg->successors.remove(then_op);

        // Replace the dependency with the relevant result from the lifted node
        dependencies[num + 1] =
            DependencyEdge{then_op, then_op->getResultForDependency(i)};

        // Remove then_op from the route for then_qid inside the block
        then_op->eraseEdgeForQID(dependency.qid.value());
      } else if (!dependency->isQuantumOp()) {
        newDeps.push_back(dependency);
      } else {
        assert(
            false &&
            "Trying to lift a quantum operation before dependency was lifted");
      }
    }

    else_op->erase();
    delete else_op;

    // Patch successors
    then_op->successors.insert(this);
    then_op->dependencies = newDeps;
  }

  /// Lifts a qubit allocated in the then/else blocks to be allocated
  /// in the graph \p parent containing this `if`. Adds a new linear argument,
  /// depending on the lifted alloc, and corresponding result, depended on by
  /// the lifted dealloc, to this `if`. If the phyiscal qubit is present in an
  /// inner block, the inner alloc/de-alloc pair will be removed. Both blocks
  /// will have the new argument/terminator dependency added, so the wire flows
  /// through the block properly even if it is not used.
  ///
  /// Currently, all allocations are lifted from `if`s, so they can be combined
  /// in the parent context. This is legal, as `if`s are treated as "solid
  /// barriers" in the parent graph, so allocating before/after the `if` is
  /// equivalent to allocating within the `if`. This effectively undoes
  /// contractAllocsPass after having allocs be contracted for performing the
  /// analysis is no longer helpful.
  ///
  /// This is not necessary to always perform, instead, one could only lift
  /// allocs when lifting operations on those allocs, but it is very difficult
  /// to do that safely.
  ///
  /// Assumes (and checks) that \p qubit is allocated in either/both the
  /// then/else blocks.
  ///
  /// If the qubit is present in both child blocks, then the extra
  /// alloc/de-alloc pair is cleaned up here.
  void liftAlloc(PhysicalQID qubit, DependencyGraph *parent) {
    InitDependencyNode *lifted_alloc = nullptr;
    DependencyNode *lifted_root = nullptr;

    bool then_contains = false;
    bool else_contains = false;

    auto then_graph = then_block->getBlockGraph();
    auto else_graph = else_block->getBlockGraph();

    // Remove virtual allocs from inner blocks
    if (else_block->getAllocatedQubits().contains(qubit)) {
      lifted_alloc = else_graph->getAllocForQubit(qubit);
      lifted_root = else_graph->getRootForQubit(qubit);
      else_block->liftAlloc(lifted_alloc->getQID(), lifted_alloc);
      else_contains = true;
    }

    if (then_block->getAllocatedQubits().contains(qubit)) {
      if (lifted_alloc)
        delete lifted_alloc;
      if (lifted_root)
        delete lifted_root;
      lifted_alloc = then_graph->getAllocForQubit(qubit);
      lifted_root = then_graph->getRootForQubit(qubit);
      then_block->liftAlloc(lifted_alloc->getQID(), lifted_alloc);
      then_contains = true;
    }

    assert(lifted_alloc && lifted_root && "Illegal qubit to lift!");

    if (!then_contains) {
      auto new_arg = then_block->addArgument(DependencyEdge{lifted_alloc, 0});
      then_block->getTerminator()->dependencies.push_back(
          DependencyEdge{new_arg, 0});
    }

    if (!else_contains) {
      auto new_arg = else_block->addArgument(DependencyEdge{lifted_alloc, 0});
      else_block->getTerminator()->dependencies.push_back(
          DependencyEdge{new_arg, 0});
    }

    // Add virtual alloc to current scope
    parent->replaceLeafAndRoot(lifted_alloc->getQID(), lifted_alloc,
                               lifted_root);
    qids.insert(lifted_alloc->getQID());
    // Hook lifted_root to the relevant result wire from this
    this->successors.insert(lifted_root);
    auto new_edge = DependencyEdge{this, results.size()};
    new_edge.qid = lifted_alloc->getQID();
    new_edge.qubit = lifted_alloc->getQubit();
    lifted_root->dependencies.push_back(new_edge);
    // Add a new result wire for the lifted wire which will flow to
    // lifted_root
    results.push_back(lifted_alloc->getResult(0).getType());
    // Hook lifted_alloc to then_op
    lifted_alloc->successors.insert(this);
    // Hook this to then_op by adding a new dependency for the lifted wire
    DependencyEdge newEdge(lifted_alloc, 0);
    dependencies.push_back(newEdge);
  }

  /// Combines physical allocations from the then and else branches
  /// by pairing them together and possibly re-indexing while respecting
  /// reuse decisions.
  void combineAllocs(SetVector<PhysicalQID> then_allocs,
                     SetVector<PhysicalQID> else_allocs) {
    SetVector<PhysicalQID> combined;
    combined.set_union(then_allocs);
    combined.set_union(else_allocs);

    // Currently, respecting reuse is enforced by combining physical wires.
    // TODO: can combine allocs in much smarter ways, possibly with heuristics,
    // to do a better job of finding lifting opportunities.
    //       To do so, would need to implement re-indexing (which is pretty
    //       trivial, an updateQubit function just like updateQID).
  }

  void genOp(OpBuilder &builder, LifeTimeAnalysis &set) override {
    cudaq::cc::IfOp oldOp = dyn_cast<cudaq::cc::IfOp>(associated);

    auto operands = gatherOperands(builder, set);

    // Remove operands from shadow dependencies
    // First operand must be conditional, skip it
    for (unsigned i = 1; i < operands.size(); i++) {
      if (!quake::isQuantumType(operands[i].getType())) {
        operands.erase(operands.begin() + i);
        i--;
      }
    }

    auto newIf =
        builder.create<cudaq::cc::IfOp>(oldOp->getLoc(), results, operands);
    auto *then_region = &newIf.getThenRegion();
    then_block->codeGen(builder, then_region, set);

    auto *else_region = &newIf.getElseRegion();
    else_block->codeGen(builder, else_region, set);

    associated = newIf;
    builder.setInsertionPointAfter(associated);
  }

  std::optional<VirtualQID> getQIDForResult(std::size_t resultidx) override {
    return then_block->getQIDForResult(resultidx);
  }

public:
  IfDependencyNode(cudaq::cc::IfOp op,
                   SmallVector<DependencyEdge> _dependencies,
                   DependencyBlock *then_block, DependencyBlock *else_block,
                   SetVector<ShadowDependencyNode *> _freevars)
      : OpDependencyNode(op.getOperation(), _dependencies),
        then_block(then_block), else_block(else_block) {
    for (auto freevar : _freevars) {
      dependencies.push_back(freevar->getShadowedEdge());
      freevars.insert(freevar);
    }

    results = SmallVector<mlir::Type>(op.getResultTypes());
    // Unfortunately, some metadata won't be computed properly by
    // OpDependencyNode constructor, so recompute here
    height = 0;
    for (auto edge : dependencies) {
      if (edge->getHeight() > height)
        height = edge->getHeight();
      if (edge.qid.has_value() && isQuantumOp())
        qids.insert(edge.qid.value());
    }
    height += numTicks();
  }

  ~IfDependencyNode() override {
    delete then_block;
    delete else_block;
  }

  unsigned numTicks() override {
    return std::max(then_block->getHeight(), else_block->getHeight());
  }

  bool isSkip() override { return numTicks() == 0; }

  bool isQuantumOp() override { return numTicks() > 0; }

  bool isContainer() override { return true; }

  SetVector<PhysicalQID> getQubits() override {
    auto qubits = SetVector<PhysicalQID>();
    qubits.set_union(then_block->getQubits());
    qubits.set_union(else_block->getQubits());
    return qubits;
  }

  void contractAllocsPass() override {
    then_block->contractAllocsPass();
    else_block->contractAllocsPass();
  }

  void eraseEdgeForQID(VirtualQID qid) override {
    // First, calculate which result to remove, but don't remove it yet
    unsigned i = 0;
    for (; i < results.size(); i++)
      if (getQIDForResult(i) == qid)
        break;

    // Erase the actual edge with the blocks now set up properly
    this->OpDependencyNode::eraseEdgeForQID(qid);

    // Now, remove the QID from the blocks so that the blocks are set up
    // properly
    then_block->removeQID(qid);
    else_block->removeQID(qid);

    // Finally, remove the calculated result, which can no longer be calculated
    // because it was removed from the blocks
    results.erase(results.begin() + i);
  }

  /// Finds and lifts common operations from the then and else branches to the
  /// parent scope. This is an optimization that a) potentially reduces the
  /// height of `if`s, and b) allows parent graphs to make more informed
  /// scheduling and reuse decisions, as information previously hidden by the
  /// "solid barrier" abstraction of `if`s is now available to them.
  void performLiftingPass() {
    bool lifted = false;

    // Currently, inner allocated qubits are always lifted in `performAnalysis`,
    // so this code is unnecessary.
    // If that becomes undesirable, uncomment the following code to allow
    // lifting of inner allocated qubits. for (auto qubit : getQubits()) {
    //   if (!then_block->getAllocatedQubits().contains(qubit) ||
    //       !else_block->getAllocatedQubits().contains(qubit))
    //     continue;
    //   auto then_use = then_block->getFirstUseOfQubit(qubit);
    //   auto else_use = else_block->getFirstUseOfQubit(qubit);

    //   if (tryLiftingBefore(then_use, else_use, parent)) {
    //     lifted = true;
    //     continue;
    //   }

    //   then_use = then_block->getLastUseOfQubit(qubit);
    //   else_use = else_block->getLastUseOfQubit(qubit);

    //   if (tryLiftingAfter(then_use, else_use, parent)) {
    //     lifted = true;
    //     continue;
    //   }
    // }

    // All qubits are lifted, so we can focus on lifting the QIDs flowing
    // through this `if`
    bool run_more = true;
    auto unliftableQIDs = SetVector<VirtualQID>();

    // Lifting operations may reveal more liftable operations!
    while (run_more) {
      run_more = false;
      auto liftableQIDs = SetVector<VirtualQID>(qids);
      liftableQIDs.set_subtract(unliftableQIDs);

      for (auto qid : liftableQIDs) {
        if (!then_block->getQIDs().contains(qid) ||
            !else_block->getQIDs().contains(qid)) {
          unliftableQIDs.insert(qid);
          continue;
        }

        auto then_graph = then_block->getBlockGraph();
        auto else_graph = else_block->getBlockGraph();

        auto then_use = then_graph->getFirstUseOfQID(qid);
        auto else_use = else_graph->getFirstUseOfQID(qid);

        // QID is no longer reference in the if, erase it
        if (!then_use || !else_use) {
          if (!then_use && !else_use)
            eraseEdgeForQID(qid);
          unliftableQIDs.insert(qid);
          continue;
        }

        if (tryLiftingBefore(then_use, else_use)) {
          lifted = true;
          run_more = true;
          continue;
        }

        then_use = then_graph->getLastUseOfQID(qid);
        else_use = else_graph->getLastUseOfQID(qid);

        if (tryLiftingAfter(then_use, else_use)) {
          lifted = true;
          run_more = true;
          continue;
        }
      }
    }

    // Recompute inner block metadata after lifting
    if (lifted) {
      then_block->updateHeight();
      else_block->updateHeight();
      then_block->schedulingPass();
      else_block->schedulingPass();
    }
  }

  /// Performs the analysis and optimizations on this `if` statement inside out:
  /// * First, recurs on the then and else blocks
  /// * Physical allocations from the two blocks are combined
  ///   and then lifted to \p parent_graph
  /// * Common operations are lifted from the blocks
  void performAnalysis(LifeTimeAnalysis &set,
                       DependencyGraph *parent_graph) override {
    // Recur first, as analysis works inside-out
    then_block->performAnalysis(set);
    // Capture allocations from then_block analysis
    auto pqids1 = set.clearFrame();
    else_block->performAnalysis(set);
    // Capture allocations from else_block analysis
    auto pqids2 = set.clearFrame();

    // Combine then and else allocations
    combineAllocs(pqids1, pqids2);

    // Lift all physical allocations out of the if to respect the if
    auto allocs = then_block->getAllocatedQubits();
    allocs.set_union(else_block->getAllocatedQubits());
    for (auto qubit : allocs)
      liftAlloc(qubit, parent_graph);

    // Lift common operations between then and else blocks
    performLiftingPass();
  }

  /// Move a virtual wire allocated and de-allocated (but not used!) from an
  /// outer scope to be allocated and de-allocated within both the then and else
  /// blocks.
  ///
  /// As a result, removes the dependency on, and result for, \p qid from this
  /// node
  void lowerAlloc(DependencyNode *init, DependencyNode *root,
                  VirtualQID qid) override {
    assert(successors.contains(root) && "Illegal root for contractAlloc");
    assert(init->successors.contains(this) && "Illegal init for contractAlloc");
    root->dependencies.erase(root->dependencies.begin());
    init->successors.clear();
    successors.remove(root);
    auto alloc = static_cast<InitDependencyNode *>(init);
    auto alloc_copy = new InitDependencyNode(*alloc);
    auto dealloc = static_cast<RootDependencyNode *>(root);
    auto dealloc_copy = new RootDependencyNode(*dealloc);
    // TODO: alloc_copy and dealloc_copy should be given a new unique QID
    //       This should be updated in the else branch as well after lowering
    std::size_t offset = getDependencyForQID(qid).value();
    associated->eraseOperand(offset);

    for (unsigned i = 0; i < results.size(); i++)
      if (getQIDForResult(i) == qid)
        results.erase(results.begin() + i);

    dependencies.erase(dependencies.begin() + offset);
    then_block->lowerAlloc(alloc, root, qid);
    else_block->lowerAlloc(alloc_copy, dealloc_copy, qid);

    // Since we're removing a result, update the result indices of successors
    for (auto successor : successors)
      for (unsigned i = 0; i < successor->dependencies.size(); i++)
        if (successor->dependencies[i].node == this &&
            successor->dependencies[i].resultidx >= offset)
          successor->dependencies[i].resultidx--;
  }
};

/// Validates that \p op meets the assumptions:
/// * operations are in linear value semantics
/// * control flow operations (except `if`s) are not allowed
/// * memory stores may be rearranged (this is not a hard error)
bool validateOp(Operation *op) {
  if (isQuakeOperation(op) && !quake::isLinearValueForm(op) &&
      !isa<quake::DiscriminateOp>(op)) {
    op->emitRemark("DependencyAnalysisPass: requires all quake operations to "
                   "be in value form. Function will be skipped");
    return false;
  }

  if (op->getRegions().size() != 0 && !isa<cudaq::cc::IfOp>(op)) {
    op->emitRemark("DependencyAnalysisPass: loops are not supported. Function "
                   "will be skipped");
    return false;
  }

  if (isa<mlir::BranchOpInterface>(op)) {
    op->emitRemark("DependencyAnalysisPass: branching operations are not "
                   "supported. Function will be skipped");
    return false;
  }

  if (isa<mlir::CallOpInterface>(op)) {
    op->emitRemark("DependencyAnalysisPass: function calls are not supported. "
                   "Function will be skipped");
    return false;
  }

  if (hasEffect<mlir::MemoryEffects::Write>(op) && !isQuakeOperation(op)) {
    op->emitWarning("DependencyAnalysisPass: memory stores are volatile and "
                    "may be reordered");
  }

  if (hasEffect<mlir::MemoryEffects::Allocate>(op) && isQuakeOperation(op) &&
      !isa<quake::BorrowWireOp>(op)) {
    op->emitRemark(
        "DependencyAnalysisPass: `quake.borrow_wire` is only "
        "supported qubit allocation operation. Function will be skipped");
    return false;
  }

  return true;
}

/// Validates that \p func meets the assumptions:
/// * function bodies contain a single block
[[maybe_unused]] bool validateFunc(func::FuncOp func) {
  if (func.getBlocks().size() != 1) {
    func.emitRemark("DependencyAnalysisPass: multiple blocks are not "
                    "supported. Function will be skipped");
    return false;
  }

  return true;
}

class DependencyAnalysisEngine {
private:
  SmallVector<DependencyNode *> perOp;
  DenseMap<BlockArgument, ArgDependencyNode *> argMap;
  SmallVector<Operation *> ifStack;
  DenseMap<Operation *, SetVector<ShadowDependencyNode *>> freeClassicals;
  unsigned vallocs;

public:
  DependencyAnalysisEngine()
      : perOp({}), argMap({}), ifStack({}), freeClassicals({}), vallocs(0) {}

  /// Creates a new dependency block for \p b by constructing a dependency graph
  /// for the body of \p b starting from the block terminator.
  ///
  /// Any operation not somehow connected to the block terminator (this will
  /// only happen if the entirety of a wire is irrelevant to the block
  /// terminator, in which case it is considered to be dead code)
  DependencyBlock *
  visitBlock(mlir::Block *b,
             SmallVector<DependencyNode::DependencyEdge> dependencies) {
    SmallVector<ArgDependencyNode *> argdnodes;
    for (auto targ : b->getArguments()) {
      ArgDependencyNode *dnode;
      // Entry block has no argument dependencies
      if (dependencies.size() > 0)
        dnode =
            new ArgDependencyNode(targ, dependencies[targ.getArgNumber() + 1]);
      else
        dnode = new ArgDependencyNode(targ);
      argMap[targ] = dnode;
      argdnodes.push_back(dnode);
    }

    DenseMap<DependencyNode *, Operation *> roots;
    TerminatorDependencyNode *terminator = nullptr;
    for (auto &op : b->getOperations()) {
      bool isTerminator = (&op == b->getTerminator());
      auto node = visitOp(&op, isTerminator);

      if (!node)
        return nullptr;

      if (isa<quake::ReturnWireOp>(&op))
        roots[node] = &op;

      if (isTerminator) {
        assert(op.hasTrait<mlir::OpTrait::IsTerminator>() &&
               "Illegal terminator op!");
        terminator = static_cast<TerminatorDependencyNode *>(node);
      }
    }

    DependencyGraph *new_graph = new DependencyGraph(terminator);
    auto included = new_graph->getRoots();

    // In debug mode, alert about dead code wires
    // TODO: If an unused wire flows through an `if` with a useful wire,
    //       then the unused wire is considered useful as the parent context
    //       doesn't know that it doesn't interact with anything inside the if,
    //       it would be nice to have a "hasInteraction" predicate inside `if`s
    //       to be able to detect this case and do a better job of removing
    //       unused wires.
    //
    //       In fact, it may be possible to completely split `if`s into various
    //       non-interacting sub-graphs, which may make solving this problem
    //       easier, and may or may not present more optimization opportunities.
    // TODO: clean up memory for unused wires.
    // Adam: I think this could be done in a silly way by placing the root
    //       in a new graph, and then deleting the graph should clean up all
    //       the nodes for the wire.
    LLVM_DEBUG(for (auto [root, op]
                    : roots) {
      if (!included.contains(root)) {
        llvm::dbgs()
            << "DependencyAnalysisPass: Wire is dead code and its "
            << "operations will be deleted (did you forget to return a value?)"
            << root << "\n";
      }
    });

    return new DependencyBlock(argdnodes, new_graph, b, terminator);
  }

  /// Creates and returns a new dependency node for \p op, connecting it to the
  /// nodes created for the defining operations of the operands of \p op
  DependencyNode *visitOp(Operation *op, bool isTerminator) {
    if (!validateOp(op))
      return nullptr;

    SmallVector<DependencyNode::DependencyEdge> dependencies;
    for (unsigned i = 0; i < op->getNumOperands(); i++)
      dependencies.push_back(visitValue(op->getOperand(i)));

    DependencyNode *newNode;

    if (auto init = dyn_cast<quake::BorrowWireOp>(op)) {
      newNode = new InitDependencyNode(init);
      vallocs++;
    } else if (auto sink = dyn_cast<quake::ReturnWireOp>(op))
      newNode = new RootDependencyNode(sink, dependencies);
    else if (auto ifop = dyn_cast<cudaq::cc::IfOp>(op)) {
      freeClassicals[op] = SetVector<ShadowDependencyNode *>();
      ifStack.push_back(op);
      auto then_block = visitBlock(ifop.getThenEntryBlock(), dependencies);
      auto else_block = visitBlock(ifop.getElseEntryBlock(), dependencies);
      if (!then_block || !else_block)
        return nullptr;
      ifStack.pop_back();

      SetVector<ShadowDependencyNode *> freeIn = freeClassicals[op];
      freeClassicals.erase(freeClassicals.find(op));

      newNode = new IfDependencyNode(ifop, dependencies, then_block, else_block,
                                     freeIn);
    } else if (isTerminator) {
      newNode = new TerminatorDependencyNode(op, dependencies);
    } else {
      newNode = new OpDependencyNode(op, dependencies);
    }

    // Dnodeid is the next slot of the dnode vector
    auto id = perOp.size();

    // Add dnodeid attribute
    OpBuilder builder(op);
    op->setAttr("dnodeid", builder.getUI32IntegerAttr(id));
    perOp.push_back(newNode);

    return newNode;
  }

  /// Returns the dependency node for the defining operation of \p v
  /// Assumption: defining operation for \p v exists and already has been
  /// visited.
  ///
  /// If \p v a classical value from a different scope, allocates a
  /// ShadowDependencyNode, and adds it to the frontier of the parent node
  /// of the use of \p v that is at the same scope as \p v
  DependencyNode::DependencyEdge visitValue(Value v) {
    if (auto barg = dyn_cast<BlockArgument>(v))
      return DependencyNode::DependencyEdge{argMap[barg], 0};

    auto defOp = v.getDefiningOp();
    assert(defOp &&
           "Cannot handle value that is neither a BlockArgument nor OpResult");

    auto resultidx = dyn_cast<OpResult>(v).getResultNumber();

    // Since we walk forward through the ast, every value should be defined
    // before it is used, so we should have already visited defOp,
    // and thus should have a memoized dnode for defOp, fail if not
    assert(defOp->hasAttr("dnodeid") && "No dnodeid found for operation");

    auto id = defOp->getAttr("dnodeid").cast<IntegerAttr>().getUInt();
    auto dnode = perOp[id];

    if (!ifStack.empty() && defOp->getParentOp() != ifStack.back() &&
        dnode->isQuantumDependent()) {
      auto opdnode = static_cast<OpDependencyNode *>(dnode);
      auto shadow_node = new ShadowDependencyNode{opdnode, resultidx};

      auto parent = ifStack.back();

      while (parent->getParentOp() != defOp->getParentOp())
        parent = parent->getParentOp();

      freeClassicals[parent].insert(shadow_node);

      return DependencyNode::DependencyEdge{shadow_node, resultidx};
    }

    return DependencyNode::DependencyEdge{dnode, resultidx};
  }

  /// Cleans up semi-constructed dependency graph when backing out of running
  /// DependencyAnalysis because of an encountered error
  void clean() {
    // TODO: clean up nodes
    // Adam: can use perOps, have to be careful about nested nodes though
  }

  unsigned getNumVirtualAllocs() { return vallocs; }
};

struct DependencyAnalysisPass
    : public cudaq::opt::impl::DependencyAnalysisBase<DependencyAnalysisPass> {
  using DependencyAnalysisBase::DependencyAnalysisBase;
  void runOnOperation() override {
    auto mod = getOperation();

    for (auto &op : mod) {
      if (auto func = dyn_cast<func::FuncOp>(op)) {
        if (!func->hasAttr("cudaq-kernel"))
          continue;

        if (func.getNumResults() == 0) {
          func->emitRemark("Function marked 'cudaq-kernel' returns no results, "
                           "qubit management skipped.");
          continue;
        }

        validateFunc(func);

        Block *oldBlock = &func.front();

        auto engine = DependencyAnalysisEngine();

        auto body = engine.visitBlock(
            oldBlock, SmallVector<DependencyNode::DependencyEdge>());

        if (!body) {
          LLVM_DEBUG(
              llvm::dbgs()
              << "Encountered issue, backing out and skipping function\n");
          engine.clean();
          continue;
        }

        OpBuilder builder(func);
        LifeTimeAnalysis set(cudaq::opt::topologyAgnosticWiresetName);
        // First, move allocs in as deep as possible. This is outside-in, so it
        // is separated from the rest of the analysis passes.
        body->contractAllocsPass();
        // Next, do the scheduling, lifetime analysis/allocation, and lifting
        // passes inside-out
        body->performAnalysis(set);
        // Finally, perform code generation to move back to quake
        body->codeGen(builder, &func.getRegion(), set);

        delete body;
        // Replace old block
        oldBlock->erase();
      }
    }
  }
};

} // namespace
