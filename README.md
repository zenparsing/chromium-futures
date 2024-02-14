# Async Features for Chromium

## Motivation

Async programming in Chromium is overly difficult. The task scheduling system and the callback
APIs are well-designed, but due to the inherent limitations of C++ and the safety mechanisms
implemented by the `base` library, async programming is far more laborious than it ought to be.

The current paradigm for async programming generally involves defining async tasks as a
collection of public and private member functions of a state-bearing class. In order to meet
memory safety requirements, the class must provide weak pointers to itself. These weak pointers
are then supplied when attaching the next continuation callback in the chain. If a weak pointer
becomes invalid, then the `Bind` API ensures that the member function will not be called.

```cpp

// A class that provides a typical asynchronous callback API.
class AsyncClass {
 public:

  // This is an async member function. It accepts a callback.
  void PerformAsyncAction(base::OnceCallback<void(int)> callback) {
    // Our first async step. Pass along a callback that will execute a
    // member function we've defined, and store the caller's callback in
    // this callback. We provide a weak pointer to ourself, so that we
    // won't get called if we've been destroyed.
    PerformAsyncStepOne(base::BindOnce(
        &DoesAsyncThings::OnAsyncStepOneCompleted, weak_factory_.GetWeakPtr(),
        std::move(callback)));
  }

 private:
  void OnAsyncStepOneCompleted(base::OnceCallback<void(int)> callback,
                               StepOneValue value) {
    // Use `value` and perform the next step in the computation, again
    // passing the user's callback along.
    PerformAsyncStepTwo(base::BindOnce(
        &DoesAsyncThings::OnAsyncStepOneCompleted, weak_factory_.GetWeakPtr(),
        std::move(callback)));
  }

  void OnAsyncStepTwoCompleted(base::OnceCallback<void(int)> callback,
                               StepTwoValue value) {
    // Use `value` and perform the next step in the computation, again
    // passing the user's callback along.
    PerformAsyncStepThree(base::BindOnce(
        &DoesAsyncThings::OnAsyncStepOneCompleted, weak_factory_.GetWeakPtr(),
        std::move(callback)));
  }

  void OnAsyncStepThreeCompleted(base::OnceCallback<void(int)> callback,
                                 int value) {
    // Finally, we can execute the callback that the user provided.
    std::move(callback).Run(value);
  }

  base::WeakPtrFactory<AsyncClass> weak_factory_{this};
};

```

> Note that `OnceCallback<>::Then` does not really help with async composition. Since it composes synchronous functions using `OnceCallback` as a wrapper, at best it can be used to avoid binding the user callback to the very last async step. It cannot eliminate any of the private member functions in the example above.

A  small class that implements a single async flow with two or three steps is manageable, but
when a class must implement more than one flow, or a flow which includes several steps, it quickly
turns into a bag of seemingly unrelated private member functions with obscure names.

Furthermore, this class-callback pattern has compositionality issues. Although tasks can be
composed by chaining callbacks together, attempts to compose tasks within a class will tend
to further obfuscate the programmer's intent. The programmer may attempt to work around this
problem by giving callback member functions increasingly long names.

The problem isn't due to the underlying facilities for async programming. The problem is that
the programmer does not have the tools to properly express the intent of an async program.

## Design Goals

New library APIs should be developed and C++ language features should be leveraged to make
the typical async programming use case easier to develop and maintain.

* The solution should occupy a layer above the scheduling and callback APIs and should not
compete with those APIs.
* It should not attempt to "fix" the callback paradigm. Instead, it should sit  on top and
provide the programmer with a new implementation option.
* It should be easy to compose async tasks.
* It should be easy to compose over existing callback APIs.
* It should be easy to provide a callback-oriented API to existing code.

Non-goals include:

* Introducing new threading primatives. The last thing an application-level Chromium engineer
needs is another way to do multithreading.
* Providing a fully generic library for utilizing C++ coroutines. Coroutines are a powerful
language-level tool that can be used to express many different semantics and solve many
different problems. We are focused on solving the callback-oriented programming problem.
Generators (using a coroutine to produce a sequence of values) and async generators (using
a coroutine to produce an asyncronous sequence of values), although interesting, should be
outside of the scope of this solution.

## Solution Idea

The solution consists of two parts, built using a layered approach:

1. Reify async values using new `Future` and `Promise` classes. These APIs will be callback-based.
2. Introduce language-level async functions by allowing coroutines to return `Future` objects.

Usage of coroutines is an implementation choice. There is no distinction, to the caller of an
API, between a function that produces a future using a coroutine and a function that produces
a future in some other way.

## Part 1: Future API

A **future** is a reification of a return value for a C++ function that does not support exceptions,
whose value may be available at some time in the future. It is a foundational building block of async
programming. A **promise** represents the capability to eventually set the value of the future.

```cpp

// Create a promise.
Promise<int> promise;

// Take the future associated with the promise. We'll typically pass
// it along to some caller.
Future<int> future = promise.GetFuture();

// Listen for the eventual value of the future.
future.AndThen(base::BindOnce([](int value) {
  LOG(INFO) << "The value of the future is: " << value;
}));

// Use the promise to set the value of the future.
promise.SetValue(42);

```

A simple future-returning function:

```cpp

// A future that settles after the specified delay.
Future<void> Delay(base::TimeDelta delta) {
  Promise<void> promise;
  Future<void> future = promise.GetFuture();

  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce([](Promise<void> p) { p.SetValue(); },
                     std::move(promise)),
      delta);

  return future;
}

```

A factory function is provided for easily adapting callback-based APIs:

```cpp

// The same, but using the `MakeFuture` factory.
Future<void> Delay(base::TimeDelta delta) {
  return MakeFuture<void>([delta](auto callback) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, std::move(callback), delta);
  });
}

```


The core future API:

```cpp

// ===========
//  Future<T>
// ===========

template <typename T>
class Future {
 public:
  using ValueType = T;

  // Futures are non-copyable.
  Future(const Future&) = delete;
  Future& operator=(const Future&) = delete;

  // Futures are movable. Moved-from futures are inactive.
  Future(Future&& other);
  Future& operator=(Future&& other);

  // Returns the value of the future, if available. If a value is returned,
  // then subsequent calls will return `std::nullopt`.
  std::optional<T> GetValueSynchronously();

  // Attaches a callback that will be executed when the future value is
  // available. The callback will be executed on the caller's task runner
  // and will always execute in a future turn. Once called, the future will
  // become inactive. It is an error to call `AndThen` on an inactive future.
  void AndThen(base::OnceCallback<void(T)> callback);

  // Attaches a transforming callback that will be executed when the future
  // value is available. Returns a future for the transformed value.
  template <typename U>
  Future<U> AndThen(base::OnceCallback<Future<U>(T)> callback);

  // Attaches a transforming callback that will be executed when the future
  // value is available. Returns a future for the transformed value.
  template <typename U>
  Future<U> Transform(base::OnceCallback<U(T)> callback);
};

// ============
//  Promise<T>
// ============

template <typename T>
class Promise {
 public:
  Promise();

  // Promises are non-copyable.
  Promise(const Promise&) = delete;
  Promise& operator=(const Promise&) = delete;

  // Promises are moveable. Moved-from promises are inactive.
  Promise(Promise&& other);
  Promise& operator=(Promise&& other);

  // Gets the associated future for this promise. It is an error to call this
  // function more than once.
  Future<T> GetFuture();

  // Sets the completed value of the associated future. Once called, the promise
  // will become inactive. It is an error to call `SetValue` on an inactive promise.
  void SetValue(T value);

  // Sets the completed value of the associated future. If a callback has been
  // registered for the associated future it will be executed synchronously.
  void SetValueWithSideEffects(T value);
};

// ================================
//  Future<void> and Promise<void>
// ================================

// Specializations for `Future<void>` and `Promise<void>` use `VoidFutureValue` as
// the underlying value type and provide convenience overloads that allow attaching
// callbacks that accept zero arguments.

struct VoidFutureValue;

template <>
class Future<void> : public Future<VoidFutureValue> {
 public:
  void AndThen(base::OnceCallback<void()> callback);

  template <typename U>
  Future<U> AndThen(base::OnceCallback<Future<U>()> callback);

  template <typename U>
  Future<U> Transform(base::OnceCallback<U()> callback);
};

template <>
class Promise<void> : public Promise<VoidFutureValue> {
 public:
  Future<void> GetFuture();
  void SetValue();
  void SetValueWithSideEffects();
};

// ===========
//  Factories
// ===========

// Returns an already-available Future for the specified value.
template <typename T>
Future<T> MakeReadyFuture(T value);

// Creates a promise/future pair, and calls the specified function with a
// callback of type `base::OnceCallback<void(Args...)>`. The Future value
// type depends upon the number of type arguments supplied, as follows:
//
// - None: `Future<void>`
// - One: `Future<T>`
// - More than one: `Future<std::tuple<Args...>>`
//
// When run, the callback function will set the value of the corresponding
// promise object. It may be called from any sequence.
template <typename... Args, typename F>
auto MakeFuture(F fn);

```

### Memory

`Future` and `Promise` do not perform dynamic memory allocation. They form an entangled
pair, where each points to the other as long as the other is alive and necessary for the
completion of the future.

### Thread-Safety and Sequences

`Future<T>` and `Promise<T>` exist to coordinate computation along a single timeline
("sequence"). They are bound to the sequence on which they were created, and are not
thread-safe. In order to coordinate async tasks across sequences, the task and callback
APIs must be used.

For both ergonomic and safety reasons, the `OnceCallback` provided by the `MakeFuture`
factory can be safely run from any sequence. It will always set the future value in the
caller's sequence.

## Part 2: Async Functions Using Coroutines

Coroutines can return `Future` objects. Within such a coroutine, the following semantics
apply:

* `co_await Future<T>`: Waits for the specified future value to become available and resumes
the coroutine with a value of type `T`.
* `co_return T`: Sets the value of the underlying promise to the specified value.
* `co_return Future<T>`: Waits for the specified future to become available and sets the
value of the underlying promise to the awaited value.

Note that `co_yield` is not supported.

A simple example:

```cpp
Future<int> AsyncWork() {
  int value = co_await MakeReadyFuture(42);
  co_return value * 2;
}
```

### Reference and Pointer Arguments

All coroutine arguments that are passed by reference or pointer - including the
implicit object reference for member functions - must either be empty or must
provide weak pointers via a `GetWeakPtr()` member function. For any such non-empty
argument, the coroutine will not resume from a `co_await` if the corresponding weak
pointer becomes invalid.

### Async Member Functions

Because Future-returning coroutines have knowledge of weak pointers, we are able to
implement the familiar async class pattern from above with a coroutine instead of
callbacks:

```cpp

class AsyncClass {
 public:
  auto GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  Future<int> PerformAsyncAction(base::OnceCallback<void(int)> callback) {
    // Note that we will not resume from co_await if we have been destroyed.
    StepOneValue step_1_value = co_await PerformAsyncStepOne();
    StepTwoValue step_2_value = co_await PerformAsyncStepTwo();
    co_return PerformAsyncStepThree();
  }

 private:
  base::WeakPtrFactory<AsyncClass> weak_factory_{this};
};

```

Notice that there is no need to create private member functions to capture each
step of the async task.

### Cancellation

In the example above, the coroutine member function will not resume if the instance
is destroyed, and the coroutine is made aware of the instance's lifetime though a
weak pointer. This is a special case of cancellation. A generalized cancellation
mechanism can be created in the following manner:

* Let `CancelToken` be a non-empty class that provides weak pointers.
* Pass a `CancelToken` object to an async function by reference.
* To cancel the task, the caller can destroy the `CancelToken` instance.
