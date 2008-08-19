// Copyright 2008, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef BASE_TASK_H__
#define BASE_TASK_H__

#include <set>

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/non_thread_safe.h"
#include "base/revocable_store.h"
#include "base/tracked.h"
#include "base/tuple.h"

//------------------------------------------------------------------------------
// Base class of Task, where we store info to help MessageLoop handle PostTask()
// elements of Task processing.

class Task;

class TaskBase : public tracked_objects::Tracked {
 public:
  TaskBase() { Reset(); }
  virtual ~TaskBase() {}

  // Use this method to adjust the priority given to a task by MessageLoop.
  void set_priority(int priority) { priority_ = priority; }
  int priority() const { return priority_; }

  // Change whether this task will run in nested message loops.
  void set_nestable(bool nestable) { nestable_ = nestable; }
  bool nestable() { return nestable_; }

  // Used to manage a linked-list of tasks.
  Task* next_task() const { return next_task_; }
  void set_next_task(Task* next) { next_task_ = next; }

 protected:
  // If a derived class wishes to re-use this instance, then it should override
  // this method.  This method is called by MessageLoop after processing a task
  // that was submitted to PostTask() or PostDelayedTask().  As seen, by default
  // it deletes the task, but the derived class can change this behaviour and
  // recycle (re-use) it.  Be sure to call Reset() if you recycle it!
  virtual void RecycleOrDelete() { delete this; }

  // Call this method if you are trying to recycle a Task.  Note that only
  // derived classes should attempt this feat, as a replacement for creating a
  // new instance.
  void Reset() {
    posted_task_delay_ = -1;
    priority_ = 0;
    next_task_ = NULL;
    nestable_ = true;
  }

 private:
  friend class TimerManager;  // To check is_owned_by_message_loop().
  friend class MessageLoop;   // To maintain posted_task_delay().

  // Access methods used ONLY by friends in MessageLoop and TimerManager
  int posted_task_delay() const { return posted_task_delay_; }
  bool is_owned_by_message_loop() const { return 0 <= posted_task_delay_; }
  void set_posted_task_delay(int delay) { posted_task_delay_ = delay; }

  // Priority for execution by MessageLoop. 0 is default. Higher means run
  // sooner, and lower (including negative) means run less soon.
  int priority_;

  // Slot to hold delay if the task was passed to PostTask().  If it was not
  // passed to PostTask, then the delay is negative (the default).
  int posted_task_delay_;

  // When tasks are collected into a queue by MessageLoop, this member is used
  // to form a null terminated list.
  Task* next_task_;

  // A nestable task will run in nested message loops, otherwise it will run
  // only in the top level message loop.
  bool nestable_;

  DISALLOW_COPY_AND_ASSIGN(TaskBase);
};


// Task ------------------------------------------------------------------------
//
// A task is a generic runnable thingy, usually used for running code on a
// different thread or for scheduling future tasks off of the message loop.

class Task : public TaskBase {
 public:
  Task() {}
  virtual ~Task() {}

  // Tasks are automatically deleted after Run is called.
  virtual void Run() = 0;
};

class CancelableTask : public Task {
 public:
  // Not all tasks support cancellation.
  virtual void Cancel() = 0;
};

// Scoped Factories ------------------------------------------------------------
//
// These scoped factory objects can be used by non-refcounted objects to safely
// place tasks in a message loop.  Each factory guarantees that the tasks it
// produces will not run after the factory is destroyed.  Commonly, factories
// are declared as class members, so the class' tasks will automatically cancel
// when the class instance is destroyed.
//
// Exampe Usage:
//
// class MyClass {
//  private:
//   // This factory will be used to schedule invocations of SomeMethod.
//   ScopedRunnableMethodFactory<MyClass> some_method_factory_;
//
//  public:
//   // It is safe to suppress warning 4355 here.
//   MyClass() : some_method_factory_(this) { }
//
//   void SomeMethod() {
//     // If this function might be called directly, you might want to revoke
//     // any outstanding runnable methods scheduled to call it.  If it's not
//     // referenced other than by the factory, this is unnecessary.
//     some_method_factory_.RevokeAll();
//     ...
//   }
//
//   void ScheduleSomeMethod() {
//     // If you'd like to only only have one pending task at a time, test for
//     // |empty| before manufacturing another task.
//     if (!some_method_factory_.empty())
//       return;
//
//     // The factories are not thread safe, so always invoke on
//     // |MessageLoop::current()|.
//     MessageLoop::current()->PostTask(FROM_HERE,
//         some_method_factory_.NewRunnableMethod(&MyClass::SomeMethod),
//         kSomeMethodDelayMS);
//   }
// };

// A ScopedTaskFactory produces tasks of type |TaskType| and prevents them from
// running after it is destroyed.
template<class TaskType>
class ScopedTaskFactory : public RevocableStore {
 public:
  ScopedTaskFactory() { }

  // Create a new task.
  inline TaskType* NewTask() {
    return new TaskWrapper(this);
  }

  class TaskWrapper : public TaskType, public NonThreadSafe {
   public:
    explicit TaskWrapper(RevocableStore* store) : revocable_(store) { }

    virtual void Run() {
      if (!revocable_.revoked())
        TaskType::Run();
    }

   private:
    Revocable revocable_;

    DISALLOW_EVIL_CONSTRUCTORS(TaskWrapper);
  };

 private:
  DISALLOW_EVIL_CONSTRUCTORS(ScopedTaskFactory);
};

// A ScopedRunnableMethodFactory creates runnable methods for a specified
// object.  This is particularly useful for generating callbacks for
// non-reference counted objects when the factory is a member of the object.
template<class T>
class ScopedRunnableMethodFactory : public RevocableStore {
 public:
  explicit ScopedRunnableMethodFactory(T* object) : object_(object) { }

  template <class Method>
  inline Task* NewRunnableMethod(Method method) {
    typedef typename ScopedTaskFactory<RunnableMethod<
        Method, Tuple0> >::TaskWrapper TaskWrapper;

    TaskWrapper* task = new TaskWrapper(this);
    task->Init(object_, method, MakeTuple());
    return task;
  }

  template <class Method, class A>
  inline Task* NewRunnableMethod(Method method, const A& a) {
    typedef typename ScopedTaskFactory<RunnableMethod<
        Method, Tuple1<A> > >::TaskWrapper TaskWrapper;

    TaskWrapper* task = new TaskWrapper(this);
    task->Init(object_, method, MakeTuple(a));
    return task;
  }

  template <class Method, class A, class B>
  inline Task* NewRunnableMethod(Method method, const A& a, const B& b) {
    typedef typename ScopedTaskFactory<RunnableMethod<
        Method, Tuple2<A, B> > >::TaskWrapper TaskWrapper;

    TaskWrapper* task = new TaskWrapper(this);
    task->Init(object_, method, MakeTuple(a, b));
    return task;
  }

  template <class Method, class A, class B, class C>
  inline Task* NewRunnableMethod(Method method,
                                 const A& a,
                                 const B& b,
                                 const C& c) {
    typedef typename ScopedTaskFactory<RunnableMethod<
        Method, Tuple3<A, B, C> > >::TaskWrapper TaskWrapper;

    TaskWrapper* task = new TaskWrapper(this);
    task->Init(object_, method, MakeTuple(a, b, c));
    return task;
  }

  template <class Method, class A, class B, class C, class D>
  inline Task* NewRunnableMethod(Method method,
                                 const A& a,
                                 const B& b,
                                 const C& c,
                                 const D& d) {
    typedef typename ScopedTaskFactory<RunnableMethod<
        Method, Tuple4<A, B, C, D> > >::TaskWrapper TaskWrapper;

    TaskWrapper* task = new TaskWrapper(this);
    task->Init(object_, method, MakeTuple(a, b, c, d));
    return task;
  }

  template <class Method, class A, class B, class C, class D, class E>
  inline Task* NewRunnableMethod(Method method,
                                 const A& a,
                                 const B& b,
                                 const C& c,
                                 const D& d,
                                 const E& e) {
    typedef typename ScopedTaskFactory<RunnableMethod<
        Method, Tuple5<A, B, C, D, E> > >::TaskWrapper TaskWrapper;

    TaskWrapper* task = new TaskWrapper(this);
    task->Init(object_, method, MakeTuple(a, b, c, d, e));
    return task;
  }

 protected:
  template <class Method, class Params>
  class RunnableMethod : public Task {
   public:
    RunnableMethod() { }

    void Init(T* obj, Method meth, const Params& params) {
      obj_ = obj;
      meth_ = meth;
      params_ = params;
    }

    virtual void Run() { DispatchToMethod(obj_, meth_, params_); }

   private:
    T* obj_;
    Method meth_;
    Params params_;

    DISALLOW_EVIL_CONSTRUCTORS(RunnableMethod);
  };

 private:
  T* object_;

  DISALLOW_EVIL_CONSTRUCTORS(ScopedRunnableMethodFactory);
};

// General task implementations ------------------------------------------------

// Task to delete an object
template<class T>
class DeleteTask : public CancelableTask {
 public:
  explicit DeleteTask(T* obj) : obj_(obj) {
    set_nestable(false);
  }
  virtual void Run() {
    delete obj_;
  }
  virtual void Cancel() {
    obj_ = NULL;
  }
 private:
  T* obj_;
};

// Task to Release() an object
template<class T>
class ReleaseTask : public CancelableTask {
 public:
  explicit ReleaseTask(T* obj) : obj_(obj) {
    set_nestable(false);
  }
  virtual void Run() {
    if (obj_)
      obj_->Release();
  }
  virtual void Cancel() {
    obj_ = NULL;
  }
 private:
  T* obj_;
};

// RunnableMethodTraits --------------------------------------------------------
//
// This traits-class is used by RunnableMethod to manage the lifetime of the
// callee object.  By default, it is assumed that the callee supports AddRef
// and Release methods.  A particular class can specialize this template to
// define other lifetime management.  For example, if the callee is known to
// live longer than the RunnableMethod object, then a RunnableMethodTraits
// struct could be defined with empty RetainCallee and ReleaseCallee methods.

template <class T>
struct RunnableMethodTraits {
  static void RetainCallee(T* obj) {
    obj->AddRef();
  }
  static void ReleaseCallee(T* obj) {
    obj->Release();
  }
};

// RunnableMethod and RunnableFunction -----------------------------------------
//
// Runnable methods are a type of task that call a function on an object when
// they are run. We implement both an object and a set of NewRunnableMethod and
// NewRunnableFunction functions for convenience. These functions are
// overloaded and will infer the template types, simplifying calling code.
//
// The template definitions all use the following names:
// T                - the class type of the object you're supplying
//                    this is not needed for the Static version of the call
// Method/Function  - the signature of a pointer to the method or function you
//                    want to call
// Param            - the parameter(s) to the method, possibly packed as a Tuple
// A                - the first parameter (if any) to the method
// B                - the second parameter (if any) to the mathod
//
// Put these all together and you get an object that can call a method whose
// signature is:
//   R T::MyFunction([A[, B]])
//
// Usage:
// PostTask(FROM_HERE, NewRunnableMethod(object, &Object::method[, a[, b]])
// PostTask(FROM_HERE, NewRunnableFunction(&function[, a[, b]])

// RunnableMethod and NewRunnableMethod implementation -------------------------

template <class T, class Method, class Params>
class RunnableMethod : public CancelableTask,
                       public RunnableMethodTraits<T> {
 public:
  RunnableMethod(T* obj, Method meth, const Params& params)
      : obj_(obj), meth_(meth), params_(params) {
    RetainCallee(obj_);
  }
  ~RunnableMethod() {
    ReleaseCallee();
  }

  virtual void Run() {
    if (obj_)
      DispatchToMethod(obj_, meth_, params_);
  }

  virtual void Cancel() {
    ReleaseCallee();
  }

 private:
  void ReleaseCallee() {
    if (obj_) {
      RunnableMethodTraits<T>::ReleaseCallee(obj_);
      obj_ = NULL;
    }
  }

  T* obj_;
  Method meth_;
  Params params_;
};

template <class T, class Method>
inline CancelableTask* NewRunnableMethod(T* object, Method method) {
  return new RunnableMethod<T, Method, Tuple0>(object, method, MakeTuple());
}

template <class T, class Method, class A>
inline CancelableTask* NewRunnableMethod(T* object, Method method, const A& a) {
  return new RunnableMethod<T, Method, Tuple1<A> >(object, method, MakeTuple(a));
}

template <class T, class Method, class A, class B>
inline CancelableTask* NewRunnableMethod(T* object, Method method,
const A& a, const B& b) {
  return new RunnableMethod<T, Method, Tuple2<A, B> >(object, method,
                                                      MakeTuple(a, b));
}

template <class T, class Method, class A, class B, class C>
inline CancelableTask* NewRunnableMethod(T* object, Method method,
                                          const A& a, const B& b, const C& c) {
  return new RunnableMethod<T, Method, Tuple3<A, B, C> >(object, method,
                                                         MakeTuple(a, b, c));
}

template <class T, class Method, class A, class B, class C, class D>
inline CancelableTask* NewRunnableMethod(T* object, Method method,
                                          const A& a, const B& b,
                                          const C& c, const D& d) {
  return new RunnableMethod<T, Method, Tuple4<A, B, C, D> >(object, method,
                                                            MakeTuple(a, b,
                                                                      c, d));
}

template <class T, class Method, class A, class B, class C, class D, class E>
inline CancelableTask* NewRunnableMethod(T* object, Method method,
                                          const A& a, const B& b,
                                          const C& c, const D& d, const E& e) {
  return new RunnableMethod<T,
                            Method,
                            Tuple5<A, B, C, D, E> >(object,
                                                    method,
                                                    MakeTuple(a, b, c, d, e));
}

// RunnableFunction and NewRunnableFunction implementation ---------------------

template <class Function, class Params>
class RunnableFunction : public CancelableTask {
 public:
  RunnableFunction(Function function, const Params& params)
      : function_(function), params_(params) {
  }

  ~RunnableFunction() {
  }

  virtual void Run() {
    if (function_)
      DispatchToFunction(function_, params_);
  }

  virtual void Cancel() {
  }

 private:
  Function function_;
  Params params_;
};

template <class Function>
inline CancelableTask* NewRunnableFunction(Function function) {
  return new RunnableFunction<Function, Tuple0>(function, MakeTuple());
}

template <class Function, class A>
inline CancelableTask* NewRunnableFunction(Function function, const A& a) {
  return new RunnableFunction<Function, Tuple1<A> >(function, MakeTuple(a));
}

template <class Function, class A, class B>
inline CancelableTask* NewRunnableFunction(Function function,
                                           const A& a, const B& b) {
  return new RunnableFunction<Function, Tuple2<A, B> >(function, MakeTuple(a, b));
}

template <class Function, class A, class B, class C>
inline CancelableTask* NewRunnableFunction(Function function,
                                           const A& a, const B& b,
                                           const C& c) {
  return new RunnableFunction<Function, Tuple3<A, B, C> >(function,
                                                          MakeTuple(a, b, c));
}

template <class Function, class A, class B, class C, class D>
inline CancelableTask* NewRunnableFunction(Function function,
                                           const A& a, const B& b,
                                           const C& c, const D& d) {
  return new RunnableFunction<Function, Tuple4<A, B, C, D> >(function,
                                                             MakeTuple(a, b,
                                                                       c, d));
}

template <class Function, class A, class B, class C, class D, class E>
inline CancelableTask* NewRunnableFunction(Function function,
                                           const A& a, const B& b,
                                           const C& c, const D& d,
                                           const E& e) {
  return new RunnableFunction<Function, Tuple5<A, B, C, D, E> >(function,
                                                                MakeTuple(a, b,
                                                                          c, d,
                                                                          e));
}

// Callback --------------------------------------------------------------------
//
// A Callback is like a Task but with unbound parameters. It is basically an
// object-oriented function pointer.
//
// Callbacks are designed to work with Tuples.  A set of helper functions and
// classes is provided to hide the Tuple details from the consumer.  Client
// code will generally work with the CallbackRunner base class, which merely
// provides a Run method and is returned by the New* functions. This allows
// users to not care which type of class implements the callback, only that it
// has a certain number and type of arguments.
//
// The implementation of this is done by CallbackImpl, which inherits
// CallbackStorage to store the data. This allows the storage of the data
// (requiring the class type T) to be hidden from users, who will want to call
// this regardless of the implementor's type T.
//
// Note that callbacks currently have no facility for cancelling or abandoning
// them. We currently handle this at a higher level for cases where this is
// necessary. The pointer in a callback must remain valid until the callback
// is made.
//
// Like Task, the callback executor is responsible for deleting the callback
// pointer once the callback has executed.
//
// Example client usage:
//   void Object::DoStuff(int, string);
//   Callback2<int, string>::Type* callback =
//       NewCallback(obj, &Object::DoStuff);
//   callback->Run(5, string("hello"));
//   delete callback;
// or, equivalently, using tuples directly:
//   CallbackRunner<Tuple2<int, string> >* callback =
//       NewCallback(obj, &Object::DoStuff);
//   callback->RunWithParams(MakeTuple(5, string("hello")));

// Base for all Callbacks that handles storage of the pointers.
template <class T, typename Method>
class CallbackStorage {
 public:
  CallbackStorage(T* obj, Method meth) : obj_(obj), meth_(meth) {
  }

 protected:
  T* obj_;
  Method meth_;
};

// Interface that is exposed to the consumer, that does the actual calling
// of the method.
template <typename Params>
class CallbackRunner {
 public:
  typedef Params TupleType;

  virtual ~CallbackRunner() {}
  virtual void RunWithParams(const Params& params) = 0;

  // Convenience functions so callers don't have to deal with Tuples.
  inline void Run() {
    RunWithParams(Tuple0());
  }

  template <typename Arg1>
  inline void Run(const Arg1& a) {
    RunWithParams(Params(a));
  }

  template <typename Arg1, typename Arg2>
  inline void Run(const Arg1& a, const Arg2& b) {
    RunWithParams(Params(a, b));
  }

  template <typename Arg1, typename Arg2, typename Arg3>
  inline void Run(const Arg1& a, const Arg2& b, const Arg3& c) {
    RunWithParams(Params(a, b, c));
  }

  template <typename Arg1, typename Arg2, typename Arg3, typename Arg4>
  inline void Run(const Arg1& a, const Arg2& b, const Arg3& c, const Arg4& d) {
    RunWithParams(Params(a, b, c, d));
  }

  template <typename Arg1, typename Arg2, typename Arg3,
            typename Arg4, typename Arg5>
  inline void Run(const Arg1& a, const Arg2& b, const Arg3& c,
                  const Arg4& d, const Arg5& e) {
    RunWithParams(Params(a, b, c, d, e));
  }
};

template <class T, typename Method, typename Params>
class CallbackImpl : public CallbackStorage<T, Method>,
                     public CallbackRunner<Params> {
 public:
  CallbackImpl(T* obj, Method meth) : CallbackStorage<T, Method>(obj, meth) {
  }
  virtual void RunWithParams(const Params& params) {
    // use "this->" to force C++ to look inside our templatized base class; see
    // Effective C++, 3rd Ed, item 43, p210 for details.
    DispatchToMethod(this->obj_, this->meth_, params);
  }
};

// 0-arg implementation
struct Callback0 {
  typedef CallbackRunner<Tuple0> Type;
};

template <class T>
typename Callback0::Type* NewCallback(T* object, void (T::*method)()) {
  return new CallbackImpl<T, void (T::*)(), Tuple0 >(object, method);
}

// 1-arg implementation
template <typename Arg1>
struct Callback1 {
  typedef CallbackRunner<Tuple1<Arg1> > Type;
};

template <class T, typename Arg1>
typename Callback1<Arg1>::Type* NewCallback(T* object, void (T::*method)(Arg1)) {
  return new CallbackImpl<T, void (T::*)(Arg1), Tuple1<Arg1> >(object, method);
}

// 2-arg implementation
template <typename Arg1, typename Arg2>
struct Callback2 {
  typedef CallbackRunner<Tuple2<Arg1, Arg2> > Type;
};

template <class T, typename Arg1, typename Arg2>
typename Callback2<Arg1, Arg2>::Type* NewCallback(
    T* object,
    void (T::*method)(Arg1, Arg2)) {
  return new CallbackImpl<T, void (T::*)(Arg1, Arg2),
      Tuple2<Arg1, Arg2> >(object, method);
}

// 3-arg implementation
template <typename Arg1, typename Arg2, typename Arg3>
struct Callback3 {
  typedef CallbackRunner<Tuple3<Arg1, Arg2, Arg3> > Type;
};

template <class T, typename Arg1, typename Arg2, typename Arg3>
typename Callback3<Arg1, Arg2, Arg3>::Type* NewCallback(
    T* object,
    void (T::*method)(Arg1, Arg2, Arg3)) {
  return new CallbackImpl<T,  void (T::*)(Arg1, Arg2, Arg3),
      Tuple3<Arg1, Arg2, Arg3> >(object, method);
}

// 4-arg implementation
template <typename Arg1, typename Arg2, typename Arg3, typename Arg4>
struct Callback4 {
  typedef CallbackRunner<Tuple4<Arg1, Arg2, Arg3, Arg4> > Type;
};

template <class T, typename Arg1, typename Arg2, typename Arg3, typename Arg4>
typename Callback4<Arg1, Arg2, Arg3, Arg4>::Type* NewCallback(
    T* object,
    void (T::*method)(Arg1, Arg2, Arg3, Arg4)) {
  return new CallbackImpl<T, void (T::*)(Arg1, Arg2, Arg3, Arg4),
      Tuple4<Arg1, Arg2, Arg3, Arg4> >(object, method);
}

// 5-arg implementation
template <typename Arg1, typename Arg2, typename Arg3,
          typename Arg4, typename Arg5>
struct Callback5 {
  typedef CallbackRunner<Tuple5<Arg1, Arg2, Arg3, Arg4, Arg5> > Type;
};

template <class T, typename Arg1, typename Arg2,
          typename Arg3, typename Arg4, typename Arg5>
typename Callback5<Arg1, Arg2, Arg3, Arg4, Arg5>::Type* NewCallback(
    T* object,
    void (T::*method)(Arg1, Arg2, Arg3, Arg4, Arg5)) {
  return new CallbackImpl<T, void (T::*)(Arg1, Arg2, Arg3, Arg4, Arg5),
      Tuple5<Arg1, Arg2, Arg3, Arg4, Arg5> >(object, method);
}

#endif  // BASE_TASK_H__
