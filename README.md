# Futures for Chromium

## Motivation

Async programming in Chromium is overly difficult. The task scheduling system and the callback
API are well-designed, but due to the inherent limitations of C++ and the safety mechanisms
implemented by the `base` library, async programming is much more laborious than we would like.

The current paradigm for async programming generally involves defining async tasks as a
collection of public and private member functions of a state-bearing class. In order to meet
memory safety requirements, the class must provide weak pointers to itself. These weak pointers
are then supplied when attaching the next continuation callback in the chain. A small class that
implements a single async flow with two or three steps is manageable, but when a class must
implement more than one flow, any of which includes several steps, it quickly turns into a bag
of seemingly unrelated callbacks.

Furthermore, this class-callback pattern has compositionality issues. Although tasks can be
composed by chaining callbacks together, attempts to compose tasks within a class will tend
to obfuscate the programmer's intent.

The problem isn't with the underlying facilities for async programming. The problem is that the
programmer does not have the tools to properly express the intent of an async program.

## API

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
// type depends upon the number of type arguments supplied as follows:
//
// - None: `Future<void>`
// - One: `Future<T>`
// - More than one: `Future<std::tuple<Args...>>`
//
// When run, the callback function will set the value of the corresponding
// promise object.
//
// Example: Adapting a callback-based API to futures
//
// Future<void> Delay(base::TimeDelta delta) {
//   return MakeFuture<void>([delta](auto callback) {
//     base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
//         FROM_HERE, std::move(callback), delta);
//   });
// }
template <typename... Args, typename F>
auto MakeFuture(F functor);

```

## Memory

`Future` and `Promise` do not perform dynamic memory allocation. They form an entangled
pair, where each points to the other as long as the other is alive and necessary for the
current future state.

## Thread-Safety and Sequences

`Future<T>` and `Promise<T>` are bound to the sequence on which they were created.
They are not thread-safe. The types `SharedFuture<T>` and `SharedPromise<T>` can be
used to pass them accross sequences.

## Coroutine Integration

Coroutines can return future objects. Within such a coroutine, the following semantics
apply:

* `co_await Future<T>`: Waits for the specified future value to become available and resumes
the coroutine with the future value of type `T`.
* `co_return T`: Sets the value of the underlying promise to the specified value.
* `co_return Future<T>`: Waits for the specified future to become available and sets the
value of the underlying promise to the future value.

Note that `co_yield` is not supported.

Example:

```cpp
Future<int> AsyncWork() {
  int value = co_await MakeReadyFuture<int>(42);
  co_return value * 2;
}
```
