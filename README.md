# Futures for Chromium

## Motivation

## API

```cpp

template <typename T>
class Future {
 public:
  using ValueType = T;

  // Non-copyable
  Future(const Future&) = delete;
  Future& operator=(const Future&) = delete;

  // Movable
  Future(Future&& other);
  Future& operator=(Future&& other);

  // Returns the value of the future, if available.
  std::optional<T> GetValueSynchronously();

  // Attaches a callback that will be executed when the future value is
  // available. The callback will be executed on the caller's task runner
  // and will always execute in a future turn.
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

template <typename T>
class Promise {
 public:
  Promise();

  // Non-copyable
  Promise(const Promise&) = delete;
  Promise& operator=(const Promise&) = delete;

  // Movable
  Promise(Promise&& other);
  Promise& operator=(Promise&& other);

  // Gets the associated future for this promise. This function may only be
  // called once.
  Future<T> GetFuture();

  // Sets the completed value of the associated future.
  void SetValue(T value);

  // Sets the completed value of the associated future. If a callback has been
  // registered for the associated future it will be executed synchronously.
  void SetValueWithSideEffects(T value);
};

```

`Future<void>` and `Promise<void>` specializations are provided as API sugar over
`Future<VoidFutureValue>` and `Promise<VoidFutureValue>`, respectively. They allow
attaching callbacks that accept zero arguments.

## Memory

`Future` and `Promise` do not perform dynamic memory allocation. They form an entangled
pair, where each points to the other as long as the other is alive and necessary for the
current future state.

## Thread-Safety and Sequences

`Future<T>` and `Promise<T>` are bound to the sequence on which they were created.
They are not thread-safe. The types `SharedFuture<T>` and `SharedPromise<T>` can be
used to pass them accross sequences sequences.

## Coroutine Integration

Coroutines and `co_await` can be leveraged to implement async functions.

Usage:

```cpp
Future<int> AsyncWork() {
  int value = co_await MakeReadyFuture<int>(42);
  co_return value * 2;
}
```
