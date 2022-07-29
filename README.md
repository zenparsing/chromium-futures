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
  absl::optional<T> GetValueSynchronously();

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
  // called once; additional calls will result in a crash.
  Future<T> GetFuture();

  // Sets the completed value of the associated future.
  void SetValue(T value);
};

```

`T` may not be `void`, a pointer type, or a reference type.

`Future` and `Promise` do not perform dynamic memory allocation.

`Future<T>` and `Promise<T>` are bound to the sequence on which they were created.
They are not thread-safe. The types `SharedFuture<T>` and `SharedPromise<T>` can be
used to use them accross sequences.

## Coroutine Integration

Coroutines and `co_await` can be leveraged to implement "async functions":

```cpp
template <typename T>
class FutureAwaiter {
 public:
  explicit FutureAwaiter(Future<T> future) : future_(std::move(future)) {}

  bool await_ready() const noexcept {
    value_ = future_.GetValueSynchronously();
    return value_.has_value();
  }

  void await_suspend(std::coroutine_handle<> handle) {
    // NOTE: Awaiter objects are alive until after `await_resume` or
    // until `handle.destroy()` is called.
    future_.AndThen(base::BindOnce(&FutureAwaiter::OnReady,
                    base::Unretained(this), handle));
  }

  T await_resume() { return std::move(*value_); }

 private:
  void OnReady(std::coroutine_handle<> handle, T value) {
    value_ = std::move(value);
    handle.resume();
  }

  Future<T> future_;
  absl::optional<T> value_;
};

template <typename T, typename... Args>
struct std::coroutine_traits<Future<T>, Args...> {
  struct promise_type : public Promise<T> {
    Future<T> get_return_object() { return GetFuture(); }

    std::suspend_never initial_suspend() const { return {}; }

    std::suspend_never final_suspend() const { return {}; }

    void return_value(T value) { SetValue(std::move(value)); }

    void return_value(Future<T> future) {
      future.AndThen(base::BindOnce([](Promise<T> promise, T value) {
        promise.SetValue(std::move(value));
      }, std::move(*this)));
    }

    void unhandled_exception() noexcept {}
  };
};

template <typename T>
auto operator co_await(Future<T> future) {
  return FutureAwaiter(std::move(future));
}
```

Usage:

```cpp
base::Future<int> AsyncWork() {
  int value = co_await base::MakeReadyFuture(42);
  co_return value * 2;
}
```


