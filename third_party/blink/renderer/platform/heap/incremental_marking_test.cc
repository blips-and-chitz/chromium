// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <initializer_list>
#include <vector>

#include "base/bind.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/heap.h"
#include "third_party/blink/renderer/platform/heap/heap_allocator.h"
#include "third_party/blink/renderer/platform/heap/heap_buildflags.h"
#include "third_party/blink/renderer/platform/heap/heap_compact.h"
#include "third_party/blink/renderer/platform/heap/heap_test_utilities.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/heap/thread_state.h"
#include "third_party/blink/renderer/platform/heap/trace_traits.h"
#include "third_party/blink/renderer/platform/heap/visitor.h"

namespace blink {
namespace incremental_marking_test {

// Visitor that expects every directly reachable object from a given backing
// store to be in the set of provided objects.
class BackingVisitor : public Visitor {
 public:
  BackingVisitor(ThreadState* state, std::vector<void*>* objects)
      : Visitor(state), objects_(objects) {}
  ~BackingVisitor() final {}

  void ProcessBackingStore(HeapObjectHeader* header) {
    EXPECT_TRUE(header->IsValid());
    EXPECT_TRUE(header->IsMarked());
    header->Unmark();
    GCInfoTable::Get()
        .GCInfoFromIndex(header->GcInfoIndex())
        ->trace(this, header->Payload());
  }

  void Visit(void* obj, TraceDescriptor desc) final {
    EXPECT_TRUE(obj);
    auto pos = std::find(objects_->begin(), objects_->end(), obj);
    if (objects_->end() != pos)
      objects_->erase(pos);
    // The garbage collector will find those objects so we can mark them.
    HeapObjectHeader* const header =
        HeapObjectHeader::FromPayload(desc.base_object_payload);
    if (!header->IsMarked())
      header->Mark();
  }

  // Unused overrides.
  void VisitWeak(void* object,
                 void** object_slot,
                 TraceDescriptor desc,
                 WeakCallback callback) final {}
  void VisitBackingStoreStrongly(void* object,
                                 void** object_slot,
                                 TraceDescriptor desc) final {}
  void VisitBackingStoreWeakly(void*,
                               void**,
                               TraceDescriptor,
                               WeakCallback,
                               void*) final {}
  void VisitBackingStoreOnly(void*, void**) final {}
  void RegisterBackingStoreCallback(void** slot,
                                    MovingObjectCallback,
                                    void* callback_data) final {}
  void RegisterWeakCallback(void* closure, WeakCallback) final {}
  void Visit(const TraceWrapperV8Reference<v8::Value>&) final {}

 private:
  std::vector<void*>* objects_;
};

// Base class for initializing worklists.
class IncrementalMarkingScopeBase {
 public:
  explicit IncrementalMarkingScopeBase(ThreadState* thread_state)
      : thread_state_(thread_state), heap_(thread_state_->Heap()) {
    if (thread_state_->IsMarkingInProgress() ||
        thread_state_->IsSweepingInProgress()) {
      PreciselyCollectGarbage();
    }
    heap_.CommitCallbackStacks();
  }

  ~IncrementalMarkingScopeBase() { heap_.DecommitCallbackStacks(); }

  ThreadHeap& heap() const { return heap_; }

 protected:
  ThreadState* const thread_state_;
  ThreadHeap& heap_;
};

class IncrementalMarkingScope : public IncrementalMarkingScopeBase {
 public:
  explicit IncrementalMarkingScope(ThreadState* thread_state)
      : IncrementalMarkingScopeBase(thread_state),
        gc_forbidden_scope_(thread_state),
        marking_worklist_(heap_.GetMarkingWorklist()),
        not_fully_constructed_worklist_(
            heap_.GetNotFullyConstructedWorklist()) {
    thread_state_->SetGCPhase(ThreadState::GCPhase::kMarking);
    ThreadState::AtomicPauseScope atomic_pause_scope_(thread_state_);
    EXPECT_TRUE(marking_worklist_->IsGlobalEmpty());
    EXPECT_TRUE(not_fully_constructed_worklist_->IsGlobalEmpty());
    thread_state->EnableIncrementalMarkingBarrier();
    thread_state->current_gc_data_.visitor = std::make_unique<MarkingVisitor>(
        thread_state, MarkingVisitor::kGlobalMarking);
  }

  ~IncrementalMarkingScope() {
    EXPECT_TRUE(marking_worklist_->IsGlobalEmpty());
    EXPECT_TRUE(not_fully_constructed_worklist_->IsGlobalEmpty());
    thread_state_->DisableIncrementalMarkingBarrier();
    // Need to clear out unused worklists that might have been polluted during
    // test.
    heap_.GetWeakCallbackWorklist()->Clear();
    thread_state_->SetGCPhase(ThreadState::GCPhase::kSweeping);
    thread_state_->SetGCPhase(ThreadState::GCPhase::kNone);
  }

  MarkingWorklist* marking_worklist() const { return marking_worklist_; }
  NotFullyConstructedWorklist* not_fully_constructed_worklist() const {
    return not_fully_constructed_worklist_;
  }

 protected:
  ThreadState::GCForbiddenScope gc_forbidden_scope_;
  MarkingWorklist* const marking_worklist_;
  NotFullyConstructedWorklist* const not_fully_constructed_worklist_;
};

// Expects that the write barrier fires for the objects passed to the
// constructor. This requires that the objects are added to the marking stack
// as well as headers being marked.
class ExpectWriteBarrierFires : public IncrementalMarkingScope {
 public:
  ExpectWriteBarrierFires(ThreadState* thread_state,
                          std::initializer_list<void*> objects)
      : IncrementalMarkingScope(thread_state),
        objects_(objects),
        backing_visitor_(thread_state_, &objects_) {
    EXPECT_TRUE(marking_worklist_->IsGlobalEmpty());
    for (void* object : objects_) {
      // Ensure that the object is in the normal arena so we can ignore backing
      // objects on the marking stack.
      CHECK(ThreadHeap::IsNormalArenaIndex(
          PageFromObject(object)->Arena()->ArenaIndex()));
      headers_.push_back(HeapObjectHeader::FromPayload(object));
      EXPECT_FALSE(headers_.back()->IsMarked());
    }
    EXPECT_FALSE(objects_.empty());
  }

  ~ExpectWriteBarrierFires() {
    EXPECT_FALSE(marking_worklist_->IsGlobalEmpty());
    MarkingItem item;
    // All objects watched should be on the marking stack.
    while (marking_worklist_->Pop(WorklistTaskId::MainThread, &item)) {
      // Inspect backing stores to allow specifying objects that are only
      // reachable through a backing store.
      if (!ThreadHeap::IsNormalArenaIndex(
              PageFromObject(item.object)->Arena()->ArenaIndex())) {
        backing_visitor_.ProcessBackingStore(
            HeapObjectHeader::FromPayload(item.object));
        continue;
      }
      auto pos = std::find(objects_.begin(), objects_.end(), item.object);
      if (objects_.end() != pos)
        objects_.erase(pos);
    }
    EXPECT_TRUE(objects_.empty());
    // All headers of objects watched should be marked at this point.
    for (HeapObjectHeader* header : headers_) {
      EXPECT_TRUE(header->IsMarked());
      header->Unmark();
    }
    EXPECT_TRUE(marking_worklist_->IsGlobalEmpty());
  }

 private:
  std::vector<void*> objects_;
  std::vector<HeapObjectHeader*> headers_;
  BackingVisitor backing_visitor_;
};

// Expects that no write barrier fires for the objects passed to the
// constructor. This requires that the marking stack stays empty and the marking
// state of the object stays the same across the lifetime of the scope.
class ExpectNoWriteBarrierFires : public IncrementalMarkingScope {
 public:
  ExpectNoWriteBarrierFires(ThreadState* thread_state,
                            std::initializer_list<void*> objects)
      : IncrementalMarkingScope(thread_state) {
    EXPECT_TRUE(marking_worklist_->IsGlobalEmpty());
    for (void* object : objects_) {
      HeapObjectHeader* header = HeapObjectHeader::FromPayload(object);
      headers_.push_back({header, header->IsMarked()});
    }
  }

  ~ExpectNoWriteBarrierFires() {
    EXPECT_TRUE(marking_worklist_->IsGlobalEmpty());
    for (const auto& pair : headers_) {
      EXPECT_EQ(pair.second, pair.first->IsMarked());
      pair.first->Unmark();
    }
  }

 private:
  std::vector<void*> objects_;
  std::vector<std::pair<HeapObjectHeader*, bool /* was marked */>> headers_;
};

class Object : public GarbageCollected<Object> {
 public:
  Object() : next_(nullptr) {}
  explicit Object(Object* next) : next_(next) {}

  void set_next(Object* next) { next_ = next; }

  bool IsMarked() const {
    return HeapObjectHeader::FromPayload(this)->IsMarked();
  }

  virtual void Trace(blink::Visitor* visitor) { visitor->Trace(next_); }

  Member<Object>& next_ref() { return next_; }

 private:
  Member<Object> next_;
};

// =============================================================================
// Basic infrastructure support. ===============================================
// =============================================================================

TEST(IncrementalMarkingTest, EnableDisableBarrier) {
  EXPECT_FALSE(ThreadState::Current()->IsIncrementalMarking());
  ThreadState::Current()->EnableIncrementalMarkingBarrier();
  EXPECT_TRUE(ThreadState::Current()->IsIncrementalMarking());
  EXPECT_TRUE(ThreadState::IsAnyIncrementalMarking());
  ThreadState::Current()->DisableIncrementalMarkingBarrier();
  EXPECT_FALSE(ThreadState::Current()->IsIncrementalMarking());
}

TEST(IncrementalMarkingTest, ManualWriteBarrierTriggersWhenMarkingIsOn) {
  auto* object = MakeGarbageCollected<Object>();
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {object});
    EXPECT_FALSE(object->IsMarked());
    MarkingVisitor::WriteBarrier(object);
    EXPECT_TRUE(object->IsMarked());
  }
}

TEST(IncrementalMarkingTest, ManualWriteBarrierBailoutWhenMarkingIsOff) {
  auto* object = MakeGarbageCollected<Object>();
  EXPECT_FALSE(object->IsMarked());
  MarkingVisitor::WriteBarrier(object);
  EXPECT_FALSE(object->IsMarked());
}

// =============================================================================
// Member<T> support. ==========================================================
// =============================================================================

TEST(IncrementalMarkingTest, MemberSetUnmarkedObject) {
  auto* parent = MakeGarbageCollected<Object>();
  auto* child = MakeGarbageCollected<Object>();
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {child});
    EXPECT_FALSE(child->IsMarked());
    parent->set_next(child);
    EXPECT_TRUE(child->IsMarked());
  }
}

TEST(IncrementalMarkingTest, MemberSetMarkedObjectNoBarrier) {
  auto* parent = MakeGarbageCollected<Object>();
  auto* child = MakeGarbageCollected<Object>();
  HeapObjectHeader::FromPayload(child)->Mark();
  {
    ExpectNoWriteBarrierFires scope(ThreadState::Current(), {child});
    parent->set_next(child);
  }
}

TEST(IncrementalMarkingTest, MemberInitializingStoreNoBarrier) {
  auto* object1 = MakeGarbageCollected<Object>();
  HeapObjectHeader* object1_header = HeapObjectHeader::FromPayload(object1);
  {
    IncrementalMarkingScope scope(ThreadState::Current());
    EXPECT_FALSE(object1_header->IsMarked());
    auto* object2 = MakeGarbageCollected<Object>(object1);
    HeapObjectHeader* object2_header = HeapObjectHeader::FromPayload(object2);
    EXPECT_FALSE(object1_header->IsMarked());
    EXPECT_FALSE(object2_header->IsMarked());
  }
}

TEST(IncrementalMarkingTest, MemberReferenceAssignMember) {
  auto* obj = MakeGarbageCollected<Object>();
  Member<Object> m1;
  Member<Object>& m2 = m1;
  Member<Object> m3(obj);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    m2 = m3;
  }
}

TEST(IncrementalMarkingTest, MemberSetDeletedValueNoBarrier) {
  Member<Object> m;
  {
    ExpectNoWriteBarrierFires scope(ThreadState::Current(), {});
    m = WTF::kHashTableDeletedValue;
  }
}

TEST(IncrementalMarkingTest, MemberCopyDeletedValueNoBarrier) {
  Member<Object> m1(WTF::kHashTableDeletedValue);
  {
    ExpectNoWriteBarrierFires scope(ThreadState::Current(), {});
    Member<Object> m2(m1);
  }
}

TEST(IncrementalMarkingTest, MemberHashTraitConstructDeletedValueNoBarrier) {
  Member<Object> m1;
  {
    ExpectNoWriteBarrierFires scope(ThreadState::Current(), {});
    HashTraits<Member<Object>>::ConstructDeletedValue(m1, false);
  }
}

TEST(IncrementalMarkingTest, MemberHashTraitIsDeletedValueNoBarrier) {
  Member<Object> m1(MakeGarbageCollected<Object>());
  {
    ExpectNoWriteBarrierFires scope(ThreadState::Current(), {});
    EXPECT_FALSE(HashTraits<Member<Object>>::IsDeletedValue(m1));
  }
}

// =============================================================================
// Mixin support. ==============================================================
// =============================================================================

namespace {

class Mixin : public GarbageCollectedMixin {
 public:
  Mixin() : next_(nullptr) {}
  virtual ~Mixin() {}

  void Trace(blink::Visitor* visitor) override { visitor->Trace(next_); }

  virtual void Bar() {}

 protected:
  Member<Object> next_;
};

class ClassWithVirtual {
 protected:
  virtual void Foo() {}
};

class Child : public GarbageCollected<Child>,
              public ClassWithVirtual,
              public Mixin {
  USING_GARBAGE_COLLECTED_MIXIN(Child);

 public:
  Child() : ClassWithVirtual(), Mixin() {}
  ~Child() override {}

  void Trace(blink::Visitor* visitor) override { Mixin::Trace(visitor); }

  void Foo() override {}
  void Bar() override {}
};

class ParentWithMixinPointer : public GarbageCollected<ParentWithMixinPointer> {
 public:
  ParentWithMixinPointer() : mixin_(nullptr) {}

  void set_mixin(Mixin* mixin) { mixin_ = mixin; }

  virtual void Trace(blink::Visitor* visitor) { visitor->Trace(mixin_); }

 protected:
  Member<Mixin> mixin_;
};

}  // namespace

TEST(IncrementalMarkingTest, WriteBarrierOnUnmarkedMixinApplication) {
  ParentWithMixinPointer* parent =
      MakeGarbageCollected<ParentWithMixinPointer>();
  auto* child = MakeGarbageCollected<Child>();
  Mixin* mixin = static_cast<Mixin*>(child);
  EXPECT_NE(static_cast<void*>(child), static_cast<void*>(mixin));
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {child});
    parent->set_mixin(mixin);
  }
}

TEST(IncrementalMarkingTest, NoWriteBarrierOnMarkedMixinApplication) {
  ParentWithMixinPointer* parent =
      MakeGarbageCollected<ParentWithMixinPointer>();
  auto* child = MakeGarbageCollected<Child>();
  HeapObjectHeader::FromPayload(child)->Mark();
  Mixin* mixin = static_cast<Mixin*>(child);
  EXPECT_NE(static_cast<void*>(child), static_cast<void*>(mixin));
  {
    ExpectNoWriteBarrierFires scope(ThreadState::Current(), {child});
    parent->set_mixin(mixin);
  }
}

// =============================================================================
// HeapVector support. =========================================================
// =============================================================================

namespace {

// HeapVector allows for insertion of container objects that can be traced but
// are themselves non-garbage collected.
class NonGarbageCollectedContainer {
  DISALLOW_NEW();

 public:
  NonGarbageCollectedContainer(Object* obj, int y) : obj_(obj), y_(y) {}

  virtual ~NonGarbageCollectedContainer() {}
  virtual void Trace(blink::Visitor* visitor) { visitor->Trace(obj_); }

 private:
  Member<Object> obj_;
  int y_;
};

class NonGarbageCollectedContainerRoot {
  DISALLOW_NEW();

 public:
  NonGarbageCollectedContainerRoot(Object* obj1, Object* obj2, int y)
      : next_(obj1, y), obj_(obj2) {}
  virtual ~NonGarbageCollectedContainerRoot() {}

  virtual void Trace(blink::Visitor* visitor) {
    visitor->Trace(next_);
    visitor->Trace(obj_);
  }

 private:
  NonGarbageCollectedContainer next_;
  Member<Object> obj_;
};

}  // namespace

TEST(IncrementalMarkingTest, HeapVectorPushBackMember) {
  auto* obj = MakeGarbageCollected<Object>();
  HeapVector<Member<Object>> vec;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    vec.push_back(obj);
  }
}

TEST(IncrementalMarkingTest, HeapVectorPushBackNonGCedContainer) {
  auto* obj = MakeGarbageCollected<Object>();
  HeapVector<NonGarbageCollectedContainer> vec;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    vec.push_back(NonGarbageCollectedContainer(obj, 1));
  }
}

TEST(IncrementalMarkingTest, HeapVectorPushBackStdPair) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapVector<std::pair<Member<Object>, Member<Object>>> vec;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    vec.push_back(std::make_pair(Member<Object>(obj1), Member<Object>(obj2)));
  }
}

TEST(IncrementalMarkingTest, HeapVectorEmplaceBackMember) {
  auto* obj = MakeGarbageCollected<Object>();
  HeapVector<Member<Object>> vec;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    vec.emplace_back(obj);
  }
}

TEST(IncrementalMarkingTest, HeapVectorEmplaceBackNonGCedContainer) {
  auto* obj = MakeGarbageCollected<Object>();
  HeapVector<NonGarbageCollectedContainer> vec;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    vec.emplace_back(obj, 1);
  }
}

TEST(IncrementalMarkingTest, HeapVectorEmplaceBackStdPair) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapVector<std::pair<Member<Object>, Member<Object>>> vec;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    vec.emplace_back(obj1, obj2);
  }
}

TEST(IncrementalMarkingTest, HeapVectorCopyMember) {
  auto* object = MakeGarbageCollected<Object>();
  HeapVector<Member<Object>> vec1;
  vec1.push_back(object);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {object});
    HeapVector<Member<Object>> vec2(vec1);
  }
}

TEST(IncrementalMarkingTest, HeapVectorCopyNonGCedContainer) {
  auto* obj = MakeGarbageCollected<Object>();
  HeapVector<NonGarbageCollectedContainer> vec1;
  vec1.emplace_back(obj, 1);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    HeapVector<NonGarbageCollectedContainer> vec2(vec1);
  }
}

TEST(IncrementalMarkingTest, HeapVectorCopyStdPair) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapVector<std::pair<Member<Object>, Member<Object>>> vec1;
  vec1.emplace_back(obj1, obj2);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    HeapVector<std::pair<Member<Object>, Member<Object>>> vec2(vec1);
  }
}

TEST(IncrementalMarkingTest, HeapVectorMoveMember) {
  auto* obj = MakeGarbageCollected<Object>();
  HeapVector<Member<Object>> vec1;
  vec1.push_back(obj);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    HeapVector<Member<Object>> vec2(std::move(vec1));
  }
}

TEST(IncrementalMarkingTest, HeapVectorMoveNonGCedContainer) {
  auto* obj = MakeGarbageCollected<Object>();
  HeapVector<NonGarbageCollectedContainer> vec1;
  vec1.emplace_back(obj, 1);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    HeapVector<NonGarbageCollectedContainer> vec2(std::move(vec1));
  }
}

TEST(IncrementalMarkingTest, HeapVectorMoveStdPair) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapVector<std::pair<Member<Object>, Member<Object>>> vec1;
  vec1.emplace_back(obj1, obj2);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    HeapVector<std::pair<Member<Object>, Member<Object>>> vec2(std::move(vec1));
  }
}

TEST(IncrementalMarkingTest, HeapVectorSwapMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapVector<Member<Object>> vec1;
  vec1.push_back(obj1);
  HeapVector<Member<Object>> vec2;
  vec2.push_back(obj2);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    std::swap(vec1, vec2);
  }
}

TEST(IncrementalMarkingTest, HeapVectorSwapNonGCedContainer) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapVector<NonGarbageCollectedContainer> vec1;
  vec1.emplace_back(obj1, 1);
  HeapVector<NonGarbageCollectedContainer> vec2;
  vec2.emplace_back(obj2, 2);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    std::swap(vec1, vec2);
  }
}

TEST(IncrementalMarkingTest, HeapVectorSwapStdPair) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapVector<std::pair<Member<Object>, Member<Object>>> vec1;
  vec1.emplace_back(obj1, nullptr);
  HeapVector<std::pair<Member<Object>, Member<Object>>> vec2;
  vec2.emplace_back(nullptr, obj2);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    std::swap(vec1, vec2);
  }
}

TEST(IncrementalMarkingTest, HeapVectorSubscriptOperator) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapVector<Member<Object>> vec;
  vec.push_back(obj1);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj2});
    EXPECT_EQ(1u, vec.size());
    EXPECT_EQ(obj1, vec[0]);
    vec[0] = obj2;
    EXPECT_EQ(obj2, vec[0]);
    EXPECT_FALSE(obj1->IsMarked());
  }
}

TEST(IncrementalMarkingTest, HeapVectorEagerTracingStopsAtMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  auto* obj3 = MakeGarbageCollected<Object>();
  obj1->set_next(obj3);
  HeapVector<NonGarbageCollectedContainerRoot> vec;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    vec.emplace_back(obj1, obj2, 3);
    // |obj3| is only reachable from |obj1| which is not eagerly traced. Only
    // objects without object headers are eagerly traced.
    EXPECT_FALSE(obj3->IsMarked());
  }
}

// =============================================================================
// HeapDoublyLinkedList support. ===============================================
// =============================================================================

namespace {

class ObjectNode : public GarbageCollected<ObjectNode>,
                   public DoublyLinkedListNode<ObjectNode> {
 public:
  explicit ObjectNode(Object* obj) : obj_(obj) {}

  void Trace(Visitor* visitor) {
    visitor->Trace(obj_);
    visitor->Trace(prev_);
    visitor->Trace(next_);
  }

 private:
  friend class WTF::DoublyLinkedListNode<ObjectNode>;

  Member<Object> obj_;
  Member<ObjectNode> prev_;
  Member<ObjectNode> next_;
};

}  // namespace

TEST(IncrementalMarkingTest, HeapDoublyLinkedListPush) {
  auto* obj = MakeGarbageCollected<Object>();
  ObjectNode* obj_node = MakeGarbageCollected<ObjectNode>(obj);
  HeapDoublyLinkedList<ObjectNode> list;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj_node});
    list.Push(obj_node);
    // |obj| will be marked once |obj_node| gets processed.
    EXPECT_FALSE(obj->IsMarked());
  }
}

TEST(IncrementalMarkingTest, HeapDoublyLinkedListAppend) {
  auto* obj = MakeGarbageCollected<Object>();
  ObjectNode* obj_node = MakeGarbageCollected<ObjectNode>(obj);
  HeapDoublyLinkedList<ObjectNode> list;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj_node});
    list.Append(obj_node);
    // |obj| will be marked once |obj_node| gets processed.
    EXPECT_FALSE(obj->IsMarked());
  }
}

// =============================================================================
// HeapDeque support. ==========================================================
// =============================================================================

TEST(IncrementalMarkingTest, HeapDequePushBackMember) {
  auto* obj = MakeGarbageCollected<Object>();
  HeapDeque<Member<Object>> deq;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    deq.push_back(obj);
  }
}

TEST(IncrementalMarkingTest, HeapDequePushFrontMember) {
  auto* obj = MakeGarbageCollected<Object>();
  HeapDeque<Member<Object>> deq;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    deq.push_front(obj);
  }
}

TEST(IncrementalMarkingTest, HeapDequeEmplaceBackMember) {
  auto* obj = MakeGarbageCollected<Object>();
  HeapDeque<Member<Object>> deq;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    deq.emplace_back(obj);
  }
}

TEST(IncrementalMarkingTest, HeapDequeEmplaceFrontMember) {
  auto* obj = MakeGarbageCollected<Object>();
  HeapDeque<Member<Object>> deq;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    deq.emplace_front(obj);
  }
}

TEST(IncrementalMarkingTest, HeapDequeCopyMember) {
  auto* object = MakeGarbageCollected<Object>();
  HeapDeque<Member<Object>> deq1;
  deq1.push_back(object);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {object});
    HeapDeque<Member<Object>> deq2(deq1);
  }
}

TEST(IncrementalMarkingTest, HeapDequeMoveMember) {
  auto* object = MakeGarbageCollected<Object>();
  HeapDeque<Member<Object>> deq1;
  deq1.push_back(object);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {object});
    HeapDeque<Member<Object>> deq2(std::move(deq1));
  }
}

TEST(IncrementalMarkingTest, HeapDequeSwapMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapDeque<Member<Object>> deq1;
  deq1.push_back(obj1);
  HeapDeque<Member<Object>> deq2;
  deq2.push_back(obj2);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    std::swap(deq1, deq2);
  }
}

// =============================================================================
// HeapHashSet support. ========================================================
// =============================================================================

namespace {

template <typename Container>
void Insert() {
  auto* obj = MakeGarbageCollected<Object>();
  Container container;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    container.insert(obj);
  }
}

template <typename Container>
void InsertNoBarrier() {
  auto* obj = MakeGarbageCollected<Object>();
  Container container;
  {
    ExpectNoWriteBarrierFires scope(ThreadState::Current(), {obj});
    container.insert(obj);
  }
}

template <typename Container>
void Copy() {
  auto* obj = MakeGarbageCollected<Object>();
  Container container1;
  container1.insert(obj);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    Container container2(container1);
    EXPECT_TRUE(container1.Contains(obj));
    EXPECT_TRUE(container2.Contains(obj));
  }
}

template <typename Container>
void CopyNoBarrier() {
  auto* obj = MakeGarbageCollected<Object>();
  Container container1;
  container1.insert(obj);
  {
    ExpectNoWriteBarrierFires scope(ThreadState::Current(), {obj});
    Container container2(container1);
    EXPECT_TRUE(container1.Contains(obj));
    EXPECT_TRUE(container2.Contains(obj));
  }
}

template <typename Container>
void Move() {
  auto* obj = MakeGarbageCollected<Object>();
  Container container1;
  Container container2;
  container1.insert(obj);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj});
    container2 = std::move(container1);
  }
}

template <typename Container>
void MoveNoBarrier() {
  auto* obj = MakeGarbageCollected<Object>();
  Container container1;
  container1.insert(obj);
  {
    ExpectNoWriteBarrierFires scope(ThreadState::Current(), {obj});
    Container container2(std::move(container1));
  }
}

template <typename Container>
void Swap() {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  Container container1;
  container1.insert(obj1);
  Container container2;
  container2.insert(obj2);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    std::swap(container1, container2);
  }
}

template <typename Container>
void SwapNoBarrier() {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  Container container1;
  container1.insert(obj1);
  Container container2;
  container2.insert(obj2);
  {
    ExpectNoWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    std::swap(container1, container2);
  }
}

}  // namespace

TEST(IncrementalMarkingTest, HeapHashSetInsert) {
  Insert<HeapHashSet<Member<Object>>>();
  // Weak references are strongified for the current cycle.
  Insert<HeapHashSet<WeakMember<Object>>>();
}

TEST(IncrementalMarkingTest, HeapHashSetCopy) {
  Copy<HeapHashSet<Member<Object>>>();
  // Weak references are strongified for the current cycle.
  Copy<HeapHashSet<WeakMember<Object>>>();
}

TEST(IncrementalMarkingTest, HeapHashSetMove) {
  Move<HeapHashSet<Member<Object>>>();
  // Weak references are strongified for the current cycle.
  Move<HeapHashSet<WeakMember<Object>>>();
}

TEST(IncrementalMarkingTest, HeapHashSetSwap) {
  Swap<HeapHashSet<Member<Object>>>();
  // Weak references are strongified for the current cycle.
  Swap<HeapHashSet<WeakMember<Object>>>();
}

class StrongWeakPair : public std::pair<Member<Object>, WeakMember<Object>> {
  DISALLOW_NEW();

  typedef std::pair<Member<Object>, WeakMember<Object>> Base;

 public:
  StrongWeakPair(Object* obj1, Object* obj2) : Base(obj1, obj2) {}

  StrongWeakPair(WTF::HashTableDeletedValueType)
      : Base(WTF::kHashTableDeletedValue, nullptr) {}

  bool IsHashTableDeletedValue() const {
    return first.IsHashTableDeletedValue();
  }

  // Trace will be called for write barrier invocations. Only strong members
  // are interesting.
  void Trace(blink::Visitor* visitor) { visitor->Trace(first); }

  // TraceInCollection will be called for weak processing.
  template <typename VisitorDispatcher>
  bool TraceInCollection(VisitorDispatcher visitor,
                         WTF::WeakHandlingFlag weakness) {
    visitor->Trace(first);
    if (weakness == WTF::kNoWeakHandling) {
      visitor->Trace(second);
    }
    return false;
  }
};

}  // namespace incremental_marking_test
}  // namespace blink

namespace WTF {

template <>
struct HashTraits<blink::incremental_marking_test::StrongWeakPair>
    : SimpleClassHashTraits<blink::incremental_marking_test::StrongWeakPair> {
  static const WTF::WeakHandlingFlag kWeakHandlingFlag = WTF::kWeakHandling;

  template <typename U = void>
  struct IsTraceableInCollection {
    static const bool value = true;
  };

  static const bool kHasIsEmptyValueFunction = true;
  static bool IsEmptyValue(
      const blink::incremental_marking_test::StrongWeakPair& value) {
    return !value.first;
  }

  static void ConstructDeletedValue(
      blink::incremental_marking_test::StrongWeakPair& slot,
      bool) {
    new (NotNull, &slot)
        blink::incremental_marking_test::StrongWeakPair(kHashTableDeletedValue);
  }

  static bool IsDeletedValue(
      const blink::incremental_marking_test::StrongWeakPair& value) {
    return value.IsHashTableDeletedValue();
  }

  template <typename VisitorDispatcher>
  static bool TraceInCollection(
      VisitorDispatcher visitor,
      blink::incremental_marking_test::StrongWeakPair& t,
      WTF::WeakHandlingFlag weakness) {
    return t.TraceInCollection(visitor, weakness);
  }
};

template <>
struct DefaultHash<blink::incremental_marking_test::StrongWeakPair> {
  typedef PairHash<blink::Member<blink::incremental_marking_test::Object>,
                   blink::WeakMember<blink::incremental_marking_test::Object>>
      Hash;
};

template <>
struct IsTraceable<blink::incremental_marking_test::StrongWeakPair> {
  static const bool value = IsTraceable<std::pair<
      blink::Member<blink::incremental_marking_test::Object>,
      blink::WeakMember<blink::incremental_marking_test::Object>>>::value;
};

}  // namespace WTF

namespace blink {
namespace incremental_marking_test {

TEST(IncrementalMarkingTest, HeapHashSetStrongWeakPair) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashSet<StrongWeakPair> set;
  {
    // Both, the weak and the strong field, are hit by the write barrier.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    set.insert(StrongWeakPair(obj1, obj2));
  }
}

TEST(IncrementalMarkingTest, HeapLinkedHashSetStrongWeakPair) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapLinkedHashSet<StrongWeakPair> set;
  {
    // Both, the weak and the strong field, are hit by the write barrier.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    set.insert(StrongWeakPair(obj1, obj2));
  }
}

// =============================================================================
// HeapLinkedHashSet support. ==================================================
// =============================================================================

TEST(IncrementalMarkingTest, HeapLinkedHashSetInsert) {
  Insert<HeapLinkedHashSet<Member<Object>>>();
  // Weak references are strongified for the current cycle.
  Insert<HeapLinkedHashSet<WeakMember<Object>>>();
}

TEST(IncrementalMarkingTest, HeapLinkedHashSetCopy) {
  Copy<HeapLinkedHashSet<Member<Object>>>();
  // Weak references are strongified for the current cycle.
  Copy<HeapLinkedHashSet<WeakMember<Object>>>();
}

TEST(IncrementalMarkingTest, HeapLinkedHashSetMove) {
  Move<HeapLinkedHashSet<Member<Object>>>();
  // Weak references are strongified for the current cycle.
  Move<HeapLinkedHashSet<WeakMember<Object>>>();
}

TEST(IncrementalMarkingTest, HeapLinkedHashSetSwap) {
  Swap<HeapLinkedHashSet<Member<Object>>>();
  // Weak references are strongified for the current cycle.
  Swap<HeapLinkedHashSet<WeakMember<Object>>>();
}

// =============================================================================
// HeapHashCountedSet support. =================================================
// =============================================================================

// HeapHashCountedSet does not support copy or move.

TEST(IncrementalMarkingTest, HeapHashCountedSetInsert) {
  Insert<HeapHashCountedSet<Member<Object>>>();
  // Weak references are strongified for the current cycle.
  Insert<HeapHashCountedSet<WeakMember<Object>>>();
}

TEST(IncrementalMarkingTest, HeapHashCountedSetSwap) {
  // HeapHashCountedSet is not move constructible so we cannot use std::swap.
  {
    auto* obj1 = MakeGarbageCollected<Object>();
    auto* obj2 = MakeGarbageCollected<Object>();
    HeapHashCountedSet<Member<Object>> container1;
    container1.insert(obj1);
    HeapHashCountedSet<Member<Object>> container2;
    container2.insert(obj2);
    {
      ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
      container1.swap(container2);
    }
  }
  {
    auto* obj1 = MakeGarbageCollected<Object>();
    auto* obj2 = MakeGarbageCollected<Object>();
    HeapHashCountedSet<WeakMember<Object>> container1;
    container1.insert(obj1);
    HeapHashCountedSet<WeakMember<Object>> container2;
    container2.insert(obj2);
    {
      // Weak references are strongified for the current cycle.
      ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
      container1.swap(container2);
    }
  }
}

// =============================================================================
// HeapHashMap support. ========================================================
// =============================================================================

TEST(IncrementalMarkingTest, HeapHashMapInsertMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, Member<Object>> map;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    map.insert(obj1, obj2);
  }
}

TEST(IncrementalMarkingTest, HeapHashMapInsertWeakMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<WeakMember<Object>, WeakMember<Object>> map;
  {
    // Weak references are strongified for the current cycle.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    map.insert(obj1, obj2);
  }
}

TEST(IncrementalMarkingTest, HeapHashMapInsertMemberWeakMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, WeakMember<Object>> map;
  {
    // Weak references are strongified for the current cycle.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    map.insert(obj1, obj2);
  }
}

TEST(IncrementalMarkingTest, HeapHashMapInsertWeakMemberMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<WeakMember<Object>, Member<Object>> map;
  {
    // Weak references are strongified for the current cycle.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    map.insert(obj1, obj2);
  }
}

TEST(IncrementalMarkingTest, HeapHashMapSetMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, Member<Object>> map;
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    map.Set(obj1, obj2);
  }
}

TEST(IncrementalMarkingTest, HeapHashMapSetMemberUpdateValue) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  auto* obj3 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, Member<Object>> map;
  map.insert(obj1, obj2);
  {
    // Only |obj3| is newly added to |map|, so we only expect the barrier to
    // fire on this one.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj3});
    map.Set(obj1, obj3);
    EXPECT_FALSE(HeapObjectHeader::FromPayload(obj1)->IsMarked());
    EXPECT_FALSE(HeapObjectHeader::FromPayload(obj2)->IsMarked());
  }
}

TEST(IncrementalMarkingTest, HeapHashMapIteratorChangeKey) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  auto* obj3 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, Member<Object>> map;
  map.insert(obj1, obj2);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj3});
    auto it = map.find(obj1);
    EXPECT_NE(map.end(), it);
    it->key = obj3;
  }
}

TEST(IncrementalMarkingTest, HeapHashMapIteratorChangeValue) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  auto* obj3 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, Member<Object>> map;
  map.insert(obj1, obj2);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj3});
    auto it = map.find(obj1);
    EXPECT_NE(map.end(), it);
    it->value = obj3;
  }
}

TEST(IncrementalMarkingTest, HeapHashMapCopyMemberMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, Member<Object>> map1;
  map1.insert(obj1, obj2);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    EXPECT_TRUE(map1.Contains(obj1));
    HeapHashMap<Member<Object>, Member<Object>> map2(map1);
    EXPECT_TRUE(map1.Contains(obj1));
    EXPECT_TRUE(map2.Contains(obj1));
  }
}

TEST(IncrementalMarkingTest, HeapHashMapCopyWeakMemberWeakMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<WeakMember<Object>, WeakMember<Object>> map1;
  map1.insert(obj1, obj2);
  {
    // Weak references are strongified for the current cycle.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    EXPECT_TRUE(map1.Contains(obj1));
    HeapHashMap<WeakMember<Object>, WeakMember<Object>> map2(map1);
    EXPECT_TRUE(map1.Contains(obj1));
    EXPECT_TRUE(map2.Contains(obj1));
  }
}

TEST(IncrementalMarkingTest, HeapHashMapCopyMemberWeakMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, WeakMember<Object>> map1;
  map1.insert(obj1, obj2);
  {
    // Weak references are strongified for the current cycle.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    EXPECT_TRUE(map1.Contains(obj1));
    HeapHashMap<Member<Object>, WeakMember<Object>> map2(map1);
    EXPECT_TRUE(map1.Contains(obj1));
    EXPECT_TRUE(map2.Contains(obj1));
  }
}

TEST(IncrementalMarkingTest, HeapHashMapCopyWeakMemberMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<WeakMember<Object>, Member<Object>> map1;
  map1.insert(obj1, obj2);
  {
    // Weak references are strongified for the current cycle.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    EXPECT_TRUE(map1.Contains(obj1));
    HeapHashMap<WeakMember<Object>, Member<Object>> map2(map1);
    EXPECT_TRUE(map1.Contains(obj1));
    EXPECT_TRUE(map2.Contains(obj1));
  }
}

TEST(IncrementalMarkingTest, HeapHashMapMoveMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, Member<Object>> map1;
  map1.insert(obj1, obj2);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    HeapHashMap<Member<Object>, Member<Object>> map2(std::move(map1));
  }
}

TEST(IncrementalMarkingTest, HeapHashMapMoveWeakMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<WeakMember<Object>, WeakMember<Object>> map1;
  map1.insert(obj1, obj2);
  {
    // Weak references are strongified for the current cycle.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    HeapHashMap<WeakMember<Object>, WeakMember<Object>> map2(std::move(map1));
  }
}

TEST(IncrementalMarkingTest, HeapHashMapMoveMemberWeakMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, WeakMember<Object>> map1;
  map1.insert(obj1, obj2);
  {
    // Weak references are strongified for the current cycle.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    HeapHashMap<Member<Object>, WeakMember<Object>> map2(std::move(map1));
  }
}

TEST(IncrementalMarkingTest, HeapHashMapMoveWeakMemberMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<WeakMember<Object>, Member<Object>> map1;
  map1.insert(obj1, obj2);
  {
    // Weak references are strongified for the current cycle.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    HeapHashMap<WeakMember<Object>, Member<Object>> map2(std::move(map1));
  }
}

TEST(IncrementalMarkingTest, HeapHashMapSwapMemberMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  auto* obj3 = MakeGarbageCollected<Object>();
  auto* obj4 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, Member<Object>> map1;
  map1.insert(obj1, obj2);
  HeapHashMap<Member<Object>, Member<Object>> map2;
  map2.insert(obj3, obj4);
  {
    ExpectWriteBarrierFires scope(ThreadState::Current(),
                                  {obj1, obj2, obj3, obj4});
    std::swap(map1, map2);
  }
}

TEST(IncrementalMarkingTest, HeapHashMapSwapWeakMemberWeakMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  auto* obj3 = MakeGarbageCollected<Object>();
  auto* obj4 = MakeGarbageCollected<Object>();
  HeapHashMap<WeakMember<Object>, WeakMember<Object>> map1;
  map1.insert(obj1, obj2);
  HeapHashMap<WeakMember<Object>, WeakMember<Object>> map2;
  map2.insert(obj3, obj4);
  {
    // Weak references are strongified for the current cycle.
    ExpectWriteBarrierFires scope(ThreadState::Current(),
                                  {obj1, obj2, obj3, obj4});
    std::swap(map1, map2);
  }
}

TEST(IncrementalMarkingTest, HeapHashMapSwapMemberWeakMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  auto* obj3 = MakeGarbageCollected<Object>();
  auto* obj4 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, WeakMember<Object>> map1;
  map1.insert(obj1, obj2);
  HeapHashMap<Member<Object>, WeakMember<Object>> map2;
  map2.insert(obj3, obj4);
  {
    // Weak references are strongified for the current cycle.
    ExpectWriteBarrierFires scope(ThreadState::Current(),
                                  {obj1, obj2, obj3, obj4});
    std::swap(map1, map2);
  }
}

TEST(IncrementalMarkingTest, HeapHashMapSwapWeakMemberMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  auto* obj3 = MakeGarbageCollected<Object>();
  auto* obj4 = MakeGarbageCollected<Object>();
  HeapHashMap<WeakMember<Object>, Member<Object>> map1;
  map1.insert(obj1, obj2);
  HeapHashMap<WeakMember<Object>, Member<Object>> map2;
  map2.insert(obj3, obj4);
  {
    // Weak references are strongified for the current cycle.
    ExpectWriteBarrierFires scope(ThreadState::Current(),
                                  {obj1, obj2, obj3, obj4});
    std::swap(map1, map2);
  }
}

TEST(IncrementalMarkingTest, HeapHashMapInsertStrongWeakPairMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  auto* obj3 = MakeGarbageCollected<Object>();
  HeapHashMap<StrongWeakPair, Member<Object>> map;
  {
    // Tests that the write barrier also fires for entities such as
    // StrongWeakPair that don't overload assignment operators in translators.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj3});
    map.insert(StrongWeakPair(obj1, obj2), obj3);
  }
}

TEST(IncrementalMarkingTest, HeapHashMapInsertMemberStrongWeakPair) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  auto* obj3 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, StrongWeakPair> map;
  {
    // Tests that the write barrier also fires for entities such as
    // StrongWeakPair that don't overload assignment operators in translators.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1, obj2});
    map.insert(obj1, StrongWeakPair(obj2, obj3));
  }
}

TEST(IncrementalMarkingTest, HeapHashMapCopyKeysToVectorMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, Member<Object>> map;
  map.insert(obj1, obj2);
  HeapVector<Member<Object>> vec;
  {
    // Only key should have its write barrier fired. A write barrier call for
    // value hints to an inefficient implementation.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj1});
    CopyKeysToVector(map, vec);
  }
}

TEST(IncrementalMarkingTest, HeapHashMapCopyValuesToVectorMember) {
  auto* obj1 = MakeGarbageCollected<Object>();
  auto* obj2 = MakeGarbageCollected<Object>();
  HeapHashMap<Member<Object>, Member<Object>> map;
  map.insert(obj1, obj2);
  HeapVector<Member<Object>> vec;
  {
    // Only value should have its write barrier fired. A write barrier call for
    // key hints to an inefficient implementation.
    ExpectWriteBarrierFires scope(ThreadState::Current(), {obj2});
    CopyValuesToVector(map, vec);
  }
}

// TODO(keishi) Non-weak hash table backings should be promptly freed but they
// are currently not because we emit write barriers for the backings, and we
// don't free marked backings.
TEST(IncrementalMarkingTest, DISABLED_WeakHashMapPromptlyFreeDisabled) {
  ThreadState* state = ThreadState::Current();
  state->SetGCState(ThreadState::kIncrementalMarkingStepScheduled);
  Persistent<Object> obj1 = MakeGarbageCollected<Object>();
  NormalPageArena* arena = static_cast<NormalPageArena*>(
      ThreadState::Current()->Heap().Arena(BlinkGC::kHashTableArenaIndex));
  CHECK(arena);
  {
    size_t before = arena->promptly_freed_size();
    // Create two maps so we don't promptly free at the allocation point.
    HeapHashMap<WeakMember<Object>, Member<Object>> weak_map1;
    HeapHashMap<WeakMember<Object>, Member<Object>> weak_map2;
    weak_map1.insert(obj1, obj1);
    weak_map2.insert(obj1, obj1);
    weak_map1.clear();
    size_t after = arena->promptly_freed_size();
    // Weak hash table backings should not be promptly freed.
    EXPECT_EQ(after, before);
  }
  {
    size_t before = arena->promptly_freed_size();
    // Create two maps so we don't promptly free at the allocation point.
    HeapHashMap<Member<Object>, Member<Object>> map1;
    HeapHashMap<Member<Object>, Member<Object>> map2;
    map1.insert(obj1, obj1);
    map2.insert(obj1, obj1);
    map1.clear();
    size_t after = arena->promptly_freed_size();
    // Non-weak hash table backings should be promptly freed.
    EXPECT_GT(after, before);
  }
  state->SetGCState(ThreadState::kIncrementalMarkingFinalizeScheduled);
  state->SetGCState(ThreadState::kNoGCScheduled);
}

namespace {

class RegisteringMixin;
using ObjectRegistry = HeapHashMap<void*, Member<RegisteringMixin>>;

class RegisteringMixin : public GarbageCollectedMixin {
 public:
  explicit RegisteringMixin(ObjectRegistry* registry) {
    HeapObjectHeader* header = GetHeapObjectHeader();
    const void* uninitialized_value = BlinkGC::kNotFullyConstructedObject;
    EXPECT_EQ(uninitialized_value, header);
    registry->insert(reinterpret_cast<void*>(this), this);
  }
};

class RegisteringObject : public GarbageCollected<RegisteringObject>,
                          public RegisteringMixin {
  USING_GARBAGE_COLLECTED_MIXIN(RegisteringObject);

 public:
  explicit RegisteringObject(ObjectRegistry* registry)
      : RegisteringMixin(registry) {}
};

}  // namespace

TEST(IncrementalMarkingTest, WriteBarrierDuringMixinConstruction) {
  IncrementalMarkingScope scope(ThreadState::Current());
  ObjectRegistry registry;
  RegisteringObject* object =
      MakeGarbageCollected<RegisteringObject>(&registry);

  // Clear any objects that have been added to the regular marking worklist in
  // the process of calling the constructor.
  EXPECT_FALSE(scope.marking_worklist()->IsGlobalEmpty());
  MarkingItem marking_item;
  while (scope.marking_worklist()->Pop(WorklistTaskId::MainThread,
                                       &marking_item)) {
    HeapObjectHeader* header =
        HeapObjectHeader::FromPayload(marking_item.object);
    if (header->IsMarked())
      header->Unmark();
  }
  EXPECT_TRUE(scope.marking_worklist()->IsGlobalEmpty());

  EXPECT_FALSE(scope.not_fully_constructed_worklist()->IsGlobalEmpty());
  NotFullyConstructedItem partial_item;
  bool found_mixin_object = false;
  // The same object may be on the marking work list because of expanding
  // and rehashing of the backing store in the registry.
  while (scope.not_fully_constructed_worklist()->Pop(WorklistTaskId::MainThread,
                                                     &partial_item)) {
    if (object == partial_item)
      found_mixin_object = true;
    HeapObjectHeader* header = HeapObjectHeader::FromPayload(partial_item);
    if (header->IsMarked())
      header->Unmark();
  }
  EXPECT_TRUE(found_mixin_object);
  EXPECT_TRUE(scope.not_fully_constructed_worklist()->IsGlobalEmpty());
}

TEST(IncrementalMarkingTest, OverrideAfterMixinConstruction) {
  ObjectRegistry registry;
  RegisteringMixin* mixin = MakeGarbageCollected<RegisteringObject>(&registry);
  HeapObjectHeader* header = mixin->GetHeapObjectHeader();
  const void* uninitialized_value = BlinkGC::kNotFullyConstructedObject;
  EXPECT_NE(uninitialized_value, header);
}

// =============================================================================
// Tests that execute complete incremental garbage collections. ================
// =============================================================================

// Test driver for incremental marking. Assumes that no stack handling is
// required.
class IncrementalMarkingTestDriver {
 public:
  explicit IncrementalMarkingTestDriver(ThreadState* thread_state)
      : thread_state_(thread_state) {}
  ~IncrementalMarkingTestDriver() {
    if (thread_state_->IsIncrementalMarking())
      FinishGC();
  }

  void Start() {
    thread_state_->IncrementalMarkingStart(
        BlinkGC::GCReason::kForcedGCForTesting);
  }

  bool SingleStep(BlinkGC::StackState stack_state =
                      BlinkGC::StackState::kNoHeapPointersOnStack) {
    CHECK(thread_state_->IsIncrementalMarking());
    if (thread_state_->GetGCState() ==
        ThreadState::kIncrementalMarkingStepScheduled) {
      thread_state_->IncrementalMarkingStep(stack_state);
      return true;
    }
    return false;
  }

  void FinishSteps(BlinkGC::StackState stack_state =
                       BlinkGC::StackState::kNoHeapPointersOnStack) {
    CHECK(thread_state_->IsIncrementalMarking());
    while (SingleStep(stack_state)) {
    }
  }

  void FinishGC() {
    CHECK(thread_state_->IsIncrementalMarking());
    FinishSteps(BlinkGC::StackState::kNoHeapPointersOnStack);
    CHECK_EQ(ThreadState::kIncrementalMarkingFinalizeScheduled,
             thread_state_->GetGCState());
    thread_state_->RunScheduledGC(BlinkGC::StackState::kNoHeapPointersOnStack);
    CHECK(!thread_state_->IsIncrementalMarking());
    thread_state_->CompleteSweep();
  }

  size_t GetHeapCompactLastFixupCount() {
    HeapCompact* compaction = ThreadState::Current()->Heap().Compaction();
    return compaction->last_fixup_count_for_testing();
  }

 private:
  ThreadState* const thread_state_;
};

TEST(IncrementalMarkingTest, TestDriver) {
  IncrementalMarkingTestDriver driver(ThreadState::Current());
  driver.Start();
  EXPECT_TRUE(ThreadState::Current()->IsIncrementalMarking());
  driver.SingleStep();
  EXPECT_TRUE(ThreadState::Current()->IsIncrementalMarking());
  driver.FinishGC();
  EXPECT_FALSE(ThreadState::Current()->IsIncrementalMarking());
}

TEST(IncrementalMarkingTest, DropBackingStore) {
  // Regression test: https://crbug.com/828537
  using WeakStore = HeapHashCountedSet<WeakMember<Object>>;

  Persistent<WeakStore> persistent(new WeakStore);
  persistent->insert(MakeGarbageCollected<Object>());
  IncrementalMarkingTestDriver driver(ThreadState::Current());
  driver.Start();
  driver.FinishSteps();
  persistent->clear();
  // Marking verifier should not crash on a black backing store with all
  // black->white edges.
  driver.FinishGC();
}

TEST(IncrementalMarkingTest, WeakCallbackDoesNotReviveDeletedValue) {
  // Regression test: https://crbug.com/870196

  // std::pair avoids treating the hashset backing as weak backing.
  using WeakStore = HeapHashCountedSet<std::pair<WeakMember<Object>, size_t>>;

  Persistent<WeakStore> persistent(new WeakStore);
  // Create at least two entries to avoid completely emptying out the data
  // structure. The values for .second are chosen to be non-null as they
  // would otherwise count as empty and be skipped during iteration after the
  // first part died.
  persistent->insert({MakeGarbageCollected<Object>(), 1});
  persistent->insert({MakeGarbageCollected<Object>(), 2});
  IncrementalMarkingTestDriver driver(ThreadState::Current());
  driver.Start();
  // The backing is not treated as weak backing and thus eagerly processed,
  // effectively registering the slots of WeakMembers.
  driver.FinishSteps();
  // The following deletes the first found entry. The second entry is left
  // untouched.
  for (auto& entries : *persistent) {
    persistent->erase(entries.key);
    break;
  }
  driver.FinishGC();

  size_t count = 0;
  for (const auto& entry : *persistent) {
    count++;
    // Use the entry to keep compilers happy.
    if (entry.key.second > 0) {
    }
  }
  CHECK_EQ(1u, count);
}

TEST(IncrementalMarkingTest, NoBackingFreeDuringIncrementalMarking) {
  // Regression test: https://crbug.com/870306
  // Only reproduces in ASAN configurations.
  using WeakStore = HeapHashCountedSet<std::pair<WeakMember<Object>, size_t>>;

  Persistent<WeakStore> persistent(new WeakStore);
  // Prefill the collection to grow backing store. A new backing store allocaton
  // would trigger the write barrier, mitigating the bug where a backing store
  // is promptly freed.
  for (size_t i = 0; i < 8; i++) {
    persistent->insert({MakeGarbageCollected<Object>(), i});
  }
  IncrementalMarkingTestDriver driver(ThreadState::Current());
  driver.Start();
  persistent->insert({MakeGarbageCollected<Object>(), 8});
  // Is not allowed to free the backing store as the previous insert may have
  // registered a slot.
  persistent->clear();
  driver.FinishSteps();
  driver.FinishGC();
}

TEST(IncrementalMarkingTest, DropReferenceWithHeapCompaction) {
  using Store = HeapHashCountedSet<Member<Object>>;

  Persistent<Store> persistent(new Store());
  persistent->insert(MakeGarbageCollected<Object>());
  IncrementalMarkingTestDriver driver(ThreadState::Current());
  HeapCompact::ScheduleCompactionGCForTesting(true);
  driver.Start();
  driver.FinishSteps();
  persistent->clear();
  // Registration of movable and updatable references should not crash because
  // if a slot have nullptr reference, it doesn't call registeration method.
  driver.FinishGC();
}

TEST(IncrementalMarkingTest, HasInlineCapacityCollectionWithHeapCompaction) {
  using Store = HeapVector<Member<Object>, 2>;

  Persistent<Store> persistent(MakeGarbageCollected<Store>());
  Persistent<Store> persistent2(MakeGarbageCollected<Store>());

  IncrementalMarkingTestDriver driver(ThreadState::Current());
  HeapCompact::ScheduleCompactionGCForTesting(true);
  persistent->push_back(MakeGarbageCollected<Object>());
  driver.Start();
  driver.FinishGC();

  // Should collect also slots that has only inline buffer and nullptr
  // references.
#if defined(ANNOTATE_CONTIGUOUS_CONTAINER)
  // When ANNOTATE_CONTIGUOUS_CONTAINER is defined, inline capacity is ignored.
  EXPECT_EQ(driver.GetHeapCompactLastFixupCount(), 1u);
#else
  EXPECT_EQ(driver.GetHeapCompactLastFixupCount(), 2u);
#endif
}

TEST(IncrementalMarkingTest, WeakHashMapHeapCompaction) {
  using Store = HeapHashCountedSet<WeakMember<Object>>;

  Persistent<Store> persistent(new Store());

  IncrementalMarkingTestDriver driver(ThreadState::Current());
  HeapCompact::ScheduleCompactionGCForTesting(true);
  driver.Start();
  driver.FinishSteps();
  persistent->insert(MakeGarbageCollected<Object>());
  driver.FinishGC();

  // Weak callback should register the slot.
  EXPECT_EQ(driver.GetHeapCompactLastFixupCount(), 1u);
}

TEST(IncrementalMarkingTest, ConservativeGCWhileCompactionScheduled) {
  using Store = HeapVector<Member<Object>>;
  Persistent<Store> persistent(MakeGarbageCollected<Store>());
  persistent->push_back(MakeGarbageCollected<Object>());

  IncrementalMarkingTestDriver driver(ThreadState::Current());
  HeapCompact::ScheduleCompactionGCForTesting(true);
  driver.Start();
  driver.FinishSteps();
  ThreadState::Current()->CollectGarbage(
      BlinkGC::kHeapPointersOnStack, BlinkGC::kAtomicMarking,
      BlinkGC::kLazySweeping, BlinkGC::GCReason::kConservativeGC);

  // Heap compaction should be canceled if incremental marking finishes with a
  // conservative GC.
  EXPECT_EQ(driver.GetHeapCompactLastFixupCount(), 0u);
}

namespace {

class ObjectWithWeakMember : public GarbageCollected<ObjectWithWeakMember> {
 public:
  ObjectWithWeakMember() = default;

  void set_object(Object* object) { object_ = object; }

  void Trace(Visitor* visitor) { visitor->Trace(object_); }

 private:
  WeakMember<Object> object_ = nullptr;
};

}  // namespace

TEST(IncrementalMarkingTest, WeakMember) {
  // Regression test: https://crbug.com/913431

  Persistent<ObjectWithWeakMember> persistent(
      MakeGarbageCollected<ObjectWithWeakMember>());
  IncrementalMarkingTestDriver driver(ThreadState::Current());
  driver.Start();
  driver.FinishSteps();
  persistent->set_object(MakeGarbageCollected<Object>());
  driver.FinishGC();
  ConservativelyCollectGarbage();
}

TEST(IncrementalMarkingTest, MemberSwap) {
  // Regression test: https://crbug.com/913431
  //
  // MemberBase::Swap may be used to swap in a not-yet-processed member into an
  // already-processed member. This leads to a stale pointer that is not marked.

  Persistent<Object> object1(MakeGarbageCollected<Object>());
  IncrementalMarkingTestDriver driver(ThreadState::Current());
  driver.Start();
  // The repro leverages the fact that initializing stores do not emit a barrier
  // (because they are still reachable from stack) to simulate the problematic
  // interleaving.
  driver.FinishSteps();
  Object* object2 =
      MakeGarbageCollected<Object>(MakeGarbageCollected<Object>());
  object2->next_ref().Swap(object1->next_ref());
  driver.FinishGC();
  ConservativelyCollectGarbage();
}

namespace {

template <typename T>
class ObjectHolder : public GarbageCollected<ObjectHolder<T>> {
 public:
  ObjectHolder() = default;

  virtual void Trace(Visitor* visitor) { visitor->Trace(holder_); }

  void set_value(T* value) { holder_ = value; }
  T* value() const { return holder_.Get(); }

 private:
  Member<T> holder_;
};

}  // namespace

TEST(IncrementalMarkingTest, StepDuringObjectConstruction) {
  // Test ensures that objects in construction are delayed for processing to
  // allow omitting write barriers on initializing stores.

  using O = ObjectWithCallbackBeforeInitializer<Object>;
  using Holder = ObjectHolder<O>;
  Persistent<Holder> holder(MakeGarbageCollected<Holder>());
  IncrementalMarkingTestDriver driver(ThreadState::Current());
  driver.Start();
  MakeGarbageCollected<O>(
      base::BindOnce(
          [](IncrementalMarkingTestDriver* driver, Holder* holder, O* thiz) {
            // Publish not-fully-constructed object |thiz| by triggering write
            // barrier for the object.
            holder->set_value(thiz);
            CHECK(HeapObjectHeader::FromPayload(holder->value())->IsValid());
            // Finish call incremental steps.
            driver->FinishSteps(BlinkGC::StackState::kHeapPointersOnStack);
          },
          &driver, holder.Get()),
      MakeGarbageCollected<Object>());
  driver.FinishGC();
  CHECK(HeapObjectHeader::FromPayload(holder->value())->IsValid());
  CHECK(HeapObjectHeader::FromPayload(holder->value()->value())->IsValid());
  PreciselyCollectGarbage();
}

TEST(IncrementalMarkingTest, StepDuringMixinObjectConstruction) {
  // Test ensures that mixin objects in construction are delayed for processing
  // to allow omitting write barriers on initializing stores.

  using Parent = ObjectWithMixinWithCallbackBeforeInitializer<Object>;
  using Mixin = MixinWithCallbackBeforeInitializer<Object>;
  using Holder = ObjectHolder<Mixin>;
  Persistent<Holder> holder(MakeGarbageCollected<Holder>());
  IncrementalMarkingTestDriver driver(ThreadState::Current());
  driver.Start();
  MakeGarbageCollected<Parent>(
      base::BindOnce(
          [](IncrementalMarkingTestDriver* driver, Holder* holder,
             Mixin* thiz) {
            // Publish not-fully-constructed object
            // |thiz| by triggering write barrier for
            // the object.
            holder->set_value(thiz);
            // Finish call incremental steps.
            driver->FinishSteps(BlinkGC::StackState::kHeapPointersOnStack);
          },
          &driver, holder.Get()),
      MakeGarbageCollected<Object>());
  driver.FinishGC();
  CHECK(holder->value()->GetHeapObjectHeader()->IsValid());
  CHECK(HeapObjectHeader::FromPayload(holder->value()->value())->IsValid());
  PreciselyCollectGarbage();
}

}  // namespace incremental_marking_test
}  // namespace blink
