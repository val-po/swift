//===--- Task.h - ABI structures for asynchronous tasks ---------*- C++ -*-===//
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
// Swift ABI describing tasks.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ABI_TASK_H
#define SWIFT_ABI_TASK_H

#include "swift/ABI/Executor.h"
#include "swift/ABI/HeapObject.h"
#include "swift/ABI/Metadata.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/Runtime/Config.h"
#include "swift/Basic/STLExtras.h"
#include "bitset"
#include "string"
#include "queue"

namespace swift {
class AsyncTask;
class AsyncContext;
class Job;
struct OpaqueValue;
struct SwiftError;
class TaskStatusRecord;
class TaskGroup;

/// A schedulable job.
class alignas(2 * alignof(void*)) Job {
protected:
  // Indices into SchedulerPrivate, for use by the runtime.
  enum {
    /// The next waiting task link, an AsyncTask that is waiting on a future.
    NextWaitingTaskIndex = 0,
  };

public:
  // Reserved for the use of the scheduler.
  void *SchedulerPrivate[2];

  JobFlags Flags;

  // We use this union to avoid having to do a second indirect branch
  // when resuming an asynchronous task, which we expect will be the
  // common case.
  union {
    // A function to run a job that isn't an AsyncTask.
    JobInvokeFunction * __ptrauth_swift_job_invoke_function RunJob;

    // A function to resume an AsyncTask.
    TaskContinuationFunction * __ptrauth_swift_task_resume_function ResumeTask;
  };

  Job(JobFlags flags, JobInvokeFunction *invoke)
      : Flags(flags), RunJob(invoke) {
    assert(!isAsyncTask() && "wrong constructor for a task");
  }

  Job(JobFlags flags, TaskContinuationFunction *invoke)
      : Flags(flags), ResumeTask(invoke) {
    assert(isAsyncTask() && "wrong constructor for a non-task job");
  }

  bool isAsyncTask() const {
    return Flags.isAsyncTask();
  }

  JobPriority getPriority() const {
    return Flags.getPriority();
  }

  /// Given that we've fully established the job context in the current
  /// thread, actually start running this job.  To establish the context
  /// correctly, call swift_job_run or runJobInExecutorContext.
  void runInFullyEstablishedContext(ExecutorRef currentExecutor);

  /// Given that we've fully established the job context in the
  /// current thread, and that the job is a simple (non-task) job,
  /// actually start running this job.
  void runSimpleInFullyEstablishedContext(ExecutorRef currentExecutor) {
    RunJob(this, currentExecutor);
  }
};

// The compiler will eventually assume these.
static_assert(sizeof(Job) == 4 * sizeof(void*),
              "Job size is wrong");
static_assert(alignof(Job) == 2 * alignof(void*),
              "Job alignment is wrong");

/// The current state of a task's status records.
class ActiveTaskStatus {
  enum : uintptr_t {
    IsCancelled = 0x1,
    IsLocked = 0x2,
    RecordMask = ~uintptr_t(IsCancelled | IsLocked)
  };

  uintptr_t Value;

public:
  constexpr ActiveTaskStatus() : Value(0) {}
  ActiveTaskStatus(TaskStatusRecord *innermostRecord,
                   bool cancelled, bool locked)
    : Value(reinterpret_cast<uintptr_t>(innermostRecord)
                + (locked ? IsLocked : 0)
                + (cancelled ? IsCancelled : 0)) {}

  /// Is the task currently cancelled?
  bool isCancelled() const { return Value & IsCancelled; }

  /// Is there an active lock on the cancellation information?
  bool isLocked() const { return Value & IsLocked; }

  /// Return the innermost cancellation record.  Code running
  /// asynchronously with this task should not access this record
  /// without having first locked it; see swift_taskCancel.
  TaskStatusRecord *getInnermostRecord() const {
    return reinterpret_cast<TaskStatusRecord*>(Value & RecordMask);
  }

  static TaskStatusRecord *getStatusRecordParent(TaskStatusRecord *ptr);

  using record_iterator =
    LinkedListIterator<TaskStatusRecord, getStatusRecordParent>;
  llvm::iterator_range<record_iterator> records() const {
    return record_iterator::rangeBeginning(getInnermostRecord());
  }
};

/// An asynchronous task.  Tasks are the analogue of threads for
/// asynchronous functions: that is, they are a persistent identity
/// for the overall async computation.
///
/// ### Fragments
/// An AsyncTask may have the following fragments:
///
///    +--------------------------+
///    | taskLocalValuesFragment  |
///    | childFragment?           |
///    | groupChildFragment?      |
///    | futureFragment?          |*
///    +--------------------------+
///
/// * The future fragment is dynamic in size, based on the future result type
///   it can hold, and thus must be the *last* fragment.
class AsyncTask : public HeapObject, public Job {
public:
  /// The context for resuming the job.  When a task is scheduled
  /// as a job, the next continuation should be installed as the
  /// ResumeTask pointer in the job header, with this serving as
  /// the context pointer.
  ///
  /// We can't protect the data in the context from being overwritten
  /// by attackers, but we can at least sign the context pointer to
  /// prevent it from being corrupted in flight.
  AsyncContext * __ptrauth_swift_task_resume_context ResumeContext;

  /// The currently-active information about cancellation.
  std::atomic<ActiveTaskStatus> Status;

  /// Reserved for the use of the task-local stack allocator.
  void *AllocatorPrivate[4];

  AsyncTask(const HeapMetadata *metadata, JobFlags flags,
            TaskContinuationFunction *run,
            AsyncContext *initialContext)
    : HeapObject(metadata), Job(flags, run),
      ResumeContext(initialContext),
      Status(ActiveTaskStatus()) {
    assert(flags.isAsyncTask());
  }

  /// Given that we've already fully established the job context
  /// in the current thread, start running this task.  To establish
  /// the job context correctly, call swift_job_run or
  /// runInExecutorContext.
  void runInFullyEstablishedContext(ExecutorRef currentExecutor) {
    ResumeTask(this, currentExecutor, ResumeContext);
  }
  
  /// Check whether this task has been cancelled.
  /// Checking this is, of course, inherently race-prone on its own.
  bool isCancelled() const {
    return Status.load(std::memory_order_relaxed).isCancelled();
  }

  // ==== Task Locals Values ---------------------------------------------------

  /// Storage fragment for task local values.
  class TaskLocalValuesFragment {
  public:
    /// Type of the pointed at `next` task local item.
    enum class NextLinkType : uintptr_t {
      /// This task is known to be a "terminal" node in the lookup of task locals.
      /// In other words, even if it had a parent, the parent (and its parents)
      /// are known to not contain any any more task locals, and thus any further
      /// search beyond this task.
      IsTerminal = 0b00,
      /// The storage pointer points at the next TaskLocalChainItem in this task.
      IsNext     = 0b01,
      /// The storage pointer points at a parent AsyncTask, in which we should
      /// continue the lookup.
      ///
      /// Note that this may not necessarily be the same as the task's parent
      /// task -- we may point to a super-parent if we know / that the parent
      /// does not "contribute" any task local values. This is to speed up
      /// lookups by skipping empty parent tasks during get(), and explained
      /// in depth in `createParentLink`.
      IsParent   = 0b11
    };

    /// Values must match `TaskLocalInheritance` declared in `TaskLocal.swift`.
    enum class TaskLocalInheritance : uint8_t {
      Default = 0,
      Never   = 1
    };

    class TaskLocalItem {
    private:
      /// Mask used for the low status bits in a task local chain item.
      static const uintptr_t statusMask = 0x03;

      /// Pointer to the next task local item; be it in this task or in a parent.
      /// Low bits encode `NextLinkType`.
      /// TaskLocalItem *next = nullptr;
      uintptr_t next;

    public:
      /// The type of the key with which this value is associated.
      const Metadata *keyType;
      /// The type of the value stored by this item.
      const Metadata *valueType;

      // Trailing storage for the value itself. The storage will be
      // uninitialized or contain an instance of \c valueType.

    private:
      explicit TaskLocalItem(const Metadata *keyType, const Metadata *valueType)
        : next(0),
          keyType(keyType),
          valueType(valueType) { }

    public:
      /// TaskLocalItem which does not by itself store any value, but only points
      /// to the nearest task-local-value containing parent's first task item.
      ///
      /// This item type is used to link to the appropriate parent task's item,
      /// when the current task itself does not have any task local values itself.
      ///
      /// When a task actually has its own task locals, it should rather point
      /// to the parent's *first* task-local item in its *last* item, extending
      /// the TaskLocalItem linked list into the appropriate parent.
      static TaskLocalItem* createParentLink(AsyncTask *task, AsyncTask *parent) {
        assert(parent);
        size_t amountToAllocate = TaskLocalItem::itemSize(/*valueType*/nullptr);
        // assert(amountToAllocate % MaximumAlignment == 0); // TODO: do we need this?
        void *allocation = malloc(amountToAllocate); // TODO: use task-local allocator

        TaskLocalItem *item =
            new(allocation) TaskLocalItem(nullptr, nullptr);

        auto parentHead = parent->localValuesFragment()->head;
        if (parentHead) {
          if (parentHead->isEmpty()) {
            switch (parentHead->getNextLinkType()) {
              case NextLinkType::IsParent:
                // it has no values, and just points to its parent,
                // therefore skip also skip pointing to that parent and point
                // to whichever parent it was pointing to as well, it may be its
                // immediate parent, or some super-parent.
                item->next = reinterpret_cast<uintptr_t>(parentHead->getNext()) |
                                  static_cast<uintptr_t>(NextLinkType::IsParent);
                break;
              case NextLinkType::IsNext:
                assert(false && "empty taskValue head in parent task, yet parent's 'head' is `IsNext`, "
                                "this should not happen, as it implies the parent must have stored some value.");
                break;
              case NextLinkType::IsTerminal:
                item->next = reinterpret_cast<uintptr_t>(parentHead->getNext()) | 
                                  static_cast<uintptr_t>(NextLinkType::IsTerminal);
                break;
            }
          } else {
            item->next = reinterpret_cast<uintptr_t>(parentHead) |
                         static_cast<uintptr_t>(NextLinkType::IsParent);
          }
        } else {
          item->next = reinterpret_cast<uintptr_t>(parentHead) |
                       static_cast<uintptr_t>(NextLinkType::IsTerminal);
        }

        return item;
      }

      static TaskLocalItem* createLink(AsyncTask *task,
                                       const Metadata *keyType,
                                       const Metadata *valueType) {
        assert(task);
        size_t amountToAllocate = TaskLocalItem::itemSize(valueType);
        // assert(amountToAllocate % MaximumAlignment == 0); // TODO: do we need this?
        void *allocation = malloc(amountToAllocate); // TODO: use task-local allocator rdar://74218679
        TaskLocalItem *item =
            new(allocation) TaskLocalItem(keyType, valueType);

        auto next = task->localValuesFragment()->head;
        auto nextLinkType = next ? NextLinkType::IsNext : NextLinkType::IsTerminal;
        item->next = reinterpret_cast<uintptr_t>(next) |
                     static_cast<uintptr_t>(nextLinkType);

        return item;
      }

      void destroy() {
        if (valueType) {
          valueType->vw_destroy(getStoragePtr());
        }
      }

      TaskLocalItem *getNext() {
        return reinterpret_cast<TaskLocalItem *>(next & ~statusMask);
      }

      NextLinkType getNextLinkType() {
        return static_cast<NextLinkType>(next & statusMask);
      }

      /// Item does not contain any actual value, and is only used to point at
      /// a specific parent item.
      bool isEmpty() {
        return !valueType;
      }

      /// Retrieve a pointer to the storage of the value.
      OpaqueValue *getStoragePtr() {
        return reinterpret_cast<OpaqueValue *>(
            reinterpret_cast<char *>(this) + storageOffset(valueType));
      }

      /// Compute the offset of the storage from the base of the item.
      static size_t storageOffset(const Metadata *valueType) {
        size_t offset = sizeof(TaskLocalItem);
        if (valueType) {
          size_t alignment = valueType->vw_alignment();
          return (offset + alignment - 1) & ~(alignment - 1);
        } else {
          return offset;
        }
      }

      /// Determine the size of the item given a particular value type.
      static size_t itemSize(const Metadata *valueType) {
        size_t offset = storageOffset(valueType);
        if (valueType) {
          offset += valueType->vw_size();
        }
        return offset;
      }
    };

  private:
    /// A stack (single-linked list) of task local values.
    ///
    /// Once task local values within this task are traversed, the list continues
    /// to the "next parent that contributes task local values," or if no such
    /// parent exists it terminates with null.
    ///
    /// If the TaskLocalValuesFragment was allocated, it is expected that this
    /// value should be NOT null; it either has own values, or at least one
    /// parent that has values. If this task does not have any values, the head
    /// pointer MAY immediately point at this task's parent task which has values.
    ///
    /// ### Concurrency
    /// Access to the head is only performed from the task itself, when it
    /// creates child tasks, the child during creation will inspect its parent's
    /// task local value stack head, and point to it. This is done on the calling
    /// task, and thus needs not to be synchronized. Subsequent traversal is
    /// performed by child tasks concurrently, however they use their own
    /// pointers/stack and can never mutate the parent's stack.
    ///
    /// The stack is only pushed/popped by the owning task, at the beginning and
    /// end a `body` block of `withLocal(_:boundTo:body:)` respectively.
    ///
    /// Correctness of the stack strongly relies on the guarantee that tasks
    /// never outline a scope in which they are created. Thanks to this, if
    /// tasks are created inside the `body` of `withLocal(_:,boundTo:body:)`
    /// all tasks created inside the `withLocal` body must complete before it
    /// returns, as such, any child tasks potentially accessing the value stack
    /// are guaranteed to be completed by the time we pop values off the stack
    /// (after the body has completed).
    TaskLocalItem *head = nullptr;

  public:
    TaskLocalValuesFragment() {}

    void destroy();

    /// If the parent task has task local values defined, point to in
    /// the task local values chain.
    void initializeLinkParent(AsyncTask* task, AsyncTask* parent);

    void pushValue(AsyncTask *task, const Metadata *keyType,
        /* +1 */ OpaqueValue *value, const Metadata *valueType);

    void popValue(AsyncTask *task);

    OpaqueValue* get(const Metadata *keType, TaskLocalInheritance inheritance);
  };

  TaskLocalValuesFragment *localValuesFragment() {
    auto offset = reinterpret_cast<char*>(this);
    offset += sizeof(AsyncTask);
    return reinterpret_cast<TaskLocalValuesFragment*>(offset);
  }

  OpaqueValue* localValueGet(const Metadata *keyType,
                             TaskLocalValuesFragment::TaskLocalInheritance inheritance) {
    return localValuesFragment()->get(keyType, inheritance);
  }

  // ==== Child Fragment -------------------------------------------------------

  /// A fragment of an async task structure that happens to be a child task.
  class ChildFragment {
    /// The parent task of this task.
    AsyncTask *Parent;

    // TODO: Document more how this is used from the `TaskGroupTaskStatusRecord`

    /// The next task in the singly-linked list of child tasks.
    /// The list must start in a `ChildTaskStatusRecord` registered
    /// with the parent task.
    ///
    /// Note that the parent task may have multiple such records.
    ///
    /// WARNING: Access can only be performed by the `Parent` of this task.
    AsyncTask *NextChild = nullptr;

  public:
    ChildFragment(AsyncTask *parent) : Parent(parent) {}

    AsyncTask *getParent() const {
      return Parent;
    }

    AsyncTask *getNextChild() const {
      return NextChild;
    }

    /// Set the `NextChild` to to the passed task.
    ///
    /// WARNING: This must ONLY be invoked from the parent of both
    /// (this and the passed-in) tasks for thread-safety reasons.
    void setNextChild(AsyncTask *task) {
      NextChild = task;
    }
  };

  bool hasChildFragment() const {
    return Flags.task_isChildTask();
  }

  ChildFragment *childFragment() {
    assert(hasChildFragment());

    auto offset = reinterpret_cast<char*>(this);
    offset += sizeof(AsyncTask);
    offset += sizeof(TaskLocalValuesFragment);

    return reinterpret_cast<ChildFragment*>(offset);
  }

  // ==== TaskGroup Child ------------------------------------------------------

  /// A child task created by `group.add` is called a "task group child."
  /// Upon completion, in addition to the usual future notifying all its waiters,
  /// it must also `group->offer` itself to the group.
  ///
  /// This signalling is necessary to correctly implement the group's `next()`.
  class GroupChildFragment {
  private:
    TaskGroup* Group;

    friend class AsyncTask;
    friend class TaskGroup;

  public:
    explicit GroupChildFragment(TaskGroup *group)
        : Group(group) {}

    /// Return the group this task should offer into when it completes.
    TaskGroup* getGroup() {
      return Group;
    }
  };

  // Checks if task is a child of a TaskGroup task.
  //
  // A child task that is a group child knows that it's parent is a group
  // and therefore may `groupOffer` to it upon completion.
  bool hasGroupChildFragment() const { return Flags.task_isGroupChildTask(); }

  GroupChildFragment *groupChildFragment() {
    assert(hasGroupChildFragment());

    auto offset = reinterpret_cast<char*>(this);
    offset += sizeof(AsyncTask);
    offset += sizeof(TaskLocalValuesFragment);
    if (hasChildFragment())
      offset += sizeof(ChildFragment);

    return reinterpret_cast<GroupChildFragment *>(offset);
  }

  // ==== Future ---------------------------------------------------------------

  class FutureFragment {
  public:
    /// Describes the status of the future.
    ///
    /// Futures always begin in the "Executing" state, and will always
    /// make a single state change to either Success or Error.
    enum class Status : uintptr_t {
      /// The future is executing or ready to execute. The storage
      /// is not accessible.
      Executing = 0,

      /// The future has completed with result (of type \c resultType).
      Success,

      /// The future has completed by throwing an error (an \c Error
      /// existential).
      Error,
    };

    /// An item within the wait queue, which includes the status and the
    /// head of the list of tasks.
    struct WaitQueueItem {
      /// Mask used for the low status bits in a wait queue item.
      static const uintptr_t statusMask = 0x03;

      uintptr_t storage;

      Status getStatus() const {
        return static_cast<Status>(storage & statusMask);
      }

      AsyncTask *getTask() const {
        return reinterpret_cast<AsyncTask *>(storage & ~statusMask);
      }

      static WaitQueueItem get(Status status, AsyncTask *task) {
        return WaitQueueItem{
          reinterpret_cast<uintptr_t>(task) | static_cast<uintptr_t>(status)};
      }
    };

  private:
    /// Queue containing all of the tasks that are waiting in `get()`.
    ///
    /// The low bits contain the status, the rest of the pointer is the
    /// AsyncTask.
    std::atomic<WaitQueueItem> waitQueue;

    /// The type of the result that will be produced by the future.
    const Metadata *resultType;

    SwiftError *error = nullptr;

    // Trailing storage for the result itself. The storage will be
    // uninitialized, contain an instance of \c resultType.

    friend class AsyncTask;

  public:
    explicit FutureFragment(const Metadata *resultType)
      : waitQueue(WaitQueueItem::get(Status::Executing, nullptr)),
        resultType(resultType) { }

    /// Destroy the storage associated with the future.
    void destroy();

    const Metadata *getResultType() const {
      return resultType;
    }

    /// Retrieve a pointer to the storage of result.
    OpaqueValue *getStoragePtr() {
      return reinterpret_cast<OpaqueValue *>(
          reinterpret_cast<char *>(this) + storageOffset(resultType));
    }

    /// Retrieve the error.
    SwiftError *&getError() { return *&error; }

    /// Compute the offset of the storage from the base of the future
    /// fragment.
    static size_t storageOffset(const Metadata *resultType)  {
      size_t offset = sizeof(FutureFragment);
      size_t alignment = resultType->vw_alignment();
      return (offset + alignment - 1) & ~(alignment - 1);
    }

    /// Determine the size of the future fragment given a particular future
    /// result type.
    static size_t fragmentSize(const Metadata *resultType) {
      return storageOffset(resultType) + resultType->vw_size();
    }
  };

  bool isFuture() const { return Flags.task_isFuture(); }

  FutureFragment *futureFragment() {
    assert(isFuture());
    auto offset = reinterpret_cast<char*>(this);
    offset += sizeof(AsyncTask);
    offset += sizeof(TaskLocalValuesFragment);
    if (hasChildFragment())
      offset += sizeof(ChildFragment);
    if (hasGroupChildFragment())
      offset += sizeof(GroupChildFragment);

    return reinterpret_cast<FutureFragment *>(offset);
  }

  /// Wait for this future to complete.
  ///
  /// \returns the status of the future. If this result is
  /// \c Executing, then \c waitingTask has been added to the
  /// wait queue and will be scheduled when the future completes. Otherwise,
  /// the future has completed and can be queried.
  FutureFragment::Status waitFuture(AsyncTask *waitingTask);

  /// Complete this future.
  ///
  /// Upon completion, any waiting tasks will be scheduled on the given
  /// executor.
  void completeFuture(AsyncContext *context, ExecutorRef executor);

  // ==== ----------------------------------------------------------------------

  static bool classof(const Job *job) {
    return job->isAsyncTask();
  }

private:
  /// Access the next waiting task, which establishes a singly linked list of
  /// tasks that are waiting on a future.
  AsyncTask *&getNextWaitingTask() {
    return reinterpret_cast<AsyncTask *&>(
        SchedulerPrivate[NextWaitingTaskIndex]);
  }

};

// The compiler will eventually assume these.
static_assert(sizeof(AsyncTask) == 12 * sizeof(void*),
              "AsyncTask size is wrong");
static_assert(alignof(AsyncTask) == 2 * alignof(void*),
              "AsyncTask alignment is wrong");

inline void Job::runInFullyEstablishedContext(ExecutorRef currentExecutor) {
  if (auto task = dyn_cast<AsyncTask>(this))
    task->runInFullyEstablishedContext(currentExecutor);
  else
    runSimpleInFullyEstablishedContext(currentExecutor);
}

/// An asynchronous context within a task.  Generally contexts are
/// allocated using the task-local stack alloc/dealloc operations, but
/// there's no guarantee of that, and the ABI is designed to permit
/// contexts to be allocated within their caller's frame.
class alignas(MaximumAlignment) AsyncContext {
public:
  /// The parent context.
  AsyncContext * __ptrauth_swift_async_context_parent Parent;

  /// The function to call to resume running in the parent context.
  /// Generally this means a semantic return, but for some temporary
  /// translation contexts it might mean initiating a call.
  ///
  /// Eventually, the actual type here will depend on the types
  /// which need to be passed to the parent.  For now, arguments
  /// are always written into the context, and so the type is
  /// always the same.
  TaskContinuationFunction * __ptrauth_swift_async_context_resume
    ResumeParent;

  /// The executor that the parent needs to be resumed on.
  ExecutorRef ResumeParentExecutor;

  /// Flags describing this context.
  ///
  /// Note that this field is only 32 bits; any alignment padding
  /// following this on 64-bit platforms can be freely used by the
  /// function.  If the function is a yielding function, that padding
  /// is of course interrupted by the YieldToParent field.
  AsyncContextFlags Flags;

  AsyncContext(AsyncContextFlags flags,
               TaskContinuationFunction *resumeParent,
               ExecutorRef resumeParentExecutor,
               AsyncContext *parent)
    : Parent(parent), ResumeParent(resumeParent),
      ResumeParentExecutor(resumeParentExecutor),
      Flags(flags) {}

  AsyncContext(const AsyncContext &) = delete;
  AsyncContext &operator=(const AsyncContext &) = delete;

  /// Perform a return from this context.
  ///
  /// Generally this should be tail-called.
  SWIFT_CC(swiftasync)
  void resumeParent(AsyncTask *task, ExecutorRef executor) {
    // TODO: destroy context before returning?
    // FIXME: force tail call
    return ResumeParent(task, executor, Parent);
  }
};

/// An async context that supports yielding.
class YieldingAsyncContext : public AsyncContext {
public:
  /// The function to call to temporarily resume running in the
  /// parent context.  Generally this means a semantic yield.
  TaskContinuationFunction * __ptrauth_swift_async_context_yield
    YieldToParent;

  /// The executor that the parent context needs to be yielded to on.
  ExecutorRef YieldToParentExecutor;

  YieldingAsyncContext(AsyncContextFlags flags,
                       TaskContinuationFunction *resumeParent,
                       ExecutorRef resumeParentExecutor,
                       TaskContinuationFunction *yieldToParent,
                       ExecutorRef yieldToParentExecutor,
                       AsyncContext *parent)
    : AsyncContext(flags, resumeParent, resumeParentExecutor, parent),
      YieldToParent(yieldToParent),
      YieldToParentExecutor(yieldToParentExecutor) {}

  static bool classof(const AsyncContext *context) {
    return context->Flags.getKind() == AsyncContextKind::Yielding;
  }
};

/// An asynchronous context within a task that describes a general "Future".
/// task.
///
/// This type matches the ABI of a function `<T> () async throws -> T`, which
/// is the type used by `Task.runDetached` and `Task.group.add` to create
/// futures.
class FutureAsyncContext : public AsyncContext {
public:
  SwiftError **errorResult = nullptr;
  OpaqueValue *indirectResult;

  using AsyncContext::AsyncContext;
};

} // end namespace swift

#endif
