/* Copyright (c) 2023 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_COMPONENTS_FUTURES_FUTURE_H_
#define BRAVE_COMPONENTS_FUTURES_FUTURE_H_

#include <coroutine>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/single_thread_task_runner.h"

namespace futures {

template <typename T>
class Promise;

// Represents the result of an asynchronous operation.
//
// Example:
//   Promise<int> promise;
//   promise.SetValue(10);
//   Future<int> future = promise.GetFuture();
//   future.AndThen(base::BindOnce([](int value) {}));
template <typename T>
class Future {
 public:
  using ValueType = T;

  Future(const Future&) = delete;
  Future& operator=(const Future&) = delete;

  Future(Future&& other) { *this = std::move(other); }

  Future& operator=(Future&& other) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (this != &other) {
      value_ = std::move(other.value_);
      promise_ptr_ = std::exchange(other.promise_ptr_, nullptr);
      if (promise_ptr_) {
        promise_ptr_->future_ptr_ = this;
      }
    }
    return *this;
  }

  ~Future() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (promise_ptr_) {
      promise_ptr_->future_ptr_ = nullptr;
    }
  }

  // Returns the value of the future, if available.
  std::optional<T> GetValueSynchronously() { return std::move(value_); }

  // Attaches a callback that will be executed when the future value is
  // available. The callback will be executed on the caller's task runner.
  void AndThen(base::OnceCallback<void(T)> callback) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (value_) {
      std::optional<T> value = std::move(value_);
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(std::move(callback), std::move(*value)));
    } else if (promise_ptr_) {
      promise_ptr_->SetCallback(std::move(callback));
      promise_ptr_ = nullptr;
    } else {
      NOTREACHED();
    }
  }

  // Attaches a transforming callback that will be executed when the future
  // value is available. Returns a future for the transformed value.
  template <typename U>
  Future<U> AndThen(base::OnceCallback<Future<U>(T)> callback) {
    Promise<U> promise;
    Future<U> future = promise.GetFuture();
    AndThen(base::BindOnce(TransformAndUnwrapFutureValue<U>, std::move(promise),
                           std::move(callback)));
    return future;
  }

  // Attaches a transforming callback that will be executed when the future
  // value is available. Returns a future for the transformed value.
  template <typename U>
  Future<U> Transform(base::OnceCallback<U(T)> callback) {
    Promise<U> promise;
    Future<U> future = promise.GetFuture();
    AndThen(base::BindOnce(TransformFutureValue<U>, std::move(promise),
                           std::move(callback)));
    return future;
  }

 private:
  friend class Promise<T>;

  Future() {}

  void SetValue(T value) {
    DCHECK(!value_);
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    value_ = std::move(value);
    promise_ptr_ = nullptr;
  }

  template <typename U>
  static void TransformFutureValue(Promise<U> promise,
                                   base::OnceCallback<U(T)> callback,
                                   T value) {
    promise.SetValueWithSideEffects(std::move(callback).Run(std::move(value)));
  }

  template <typename U>
  static void TransformAndUnwrapFutureValue(
      Promise<U> promise,
      base::OnceCallback<Future<U>(T)> callback,
      T value) {
    std::move(callback)
        .Run(std::move(value))
        .AndThen(base::BindOnce(UnwrapFutureValue<U>, std::move(promise)));
  }

  template <typename U>
  static void UnwrapFutureValue(Promise<U> promise, U value) {
    promise.SetValueWithSideEffects(std::move(value));
  }

  SEQUENCE_CHECKER(sequence_checker_);
  std::optional<T> value_;
  raw_ptr<Promise<T>> promise_ptr_ = nullptr;
};

template <typename T>
class Promise {
 public:
  Promise() : future_(Future<T>()), future_ptr_(&future_.value()) {
    future_ptr_->promise_ptr_ = this;
  }

  Promise(const Promise&) = delete;
  Promise& operator=(const Promise&) = delete;

  Promise(Promise&& other) { *this = std::move(other); }

  Promise& operator=(Promise&& other) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (this != &other) {
      future_ = std::move(other.future_);
      future_ptr_ = std::exchange(other.future_ptr_, nullptr);
      callback_ = std::move(other.callback_);
      if (future_ptr_) {
        future_ptr_->promise_ptr_ = this;
      }
    }
    return *this;
  }

  ~Promise() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (future_ptr_) {
      future_ptr_->promise_ptr_ = nullptr;
    }
  }

  // Gets the associated future for this promise. This function may only be
  // called once; additional calls will result in a crash.
  Future<T> GetFuture() {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    CHECK(future_);
    Future<T> future(std::move(*future_));
    future_.reset();
    return future;
  }

  // Sets the completed value of the associated future.
  void SetValue(T value) { SetValue(std::move(value), false); }

  // Sets the completed value of the associated future. If a callback has been
  // registered for the associated future it will be executed synchronously.
  void SetValueWithSideEffects(T value) { SetValue(std::move(value), true); }

 private:
  friend class Future<T>;

  void SetCallback(base::OnceCallback<void(T)> callback) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    DCHECK(!callback_);
    callback_ = std::move(callback);
    future_ptr_ = nullptr;
  }

  void SetValue(T value, bool with_side_effects) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (callback_) {
      if (with_side_effects) {
        std::move(callback_).Run(std::move(value));
      } else {
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(std::move(callback_), std::move(value)));
      }
    } else if (future_ptr_) {
      future_ptr_->SetValue(std::move(value));
      future_ptr_ = nullptr;
    } else {
      NOTREACHED();
    }
  }

  SEQUENCE_CHECKER(sequence_checker_);
  std::optional<Future<T>> future_;
  raw_ptr<Future<T>> future_ptr_ = nullptr;
  base::OnceCallback<void(T)> callback_;
};

struct VoidFutureValue {};

template <>
class Future<void> : public Future<VoidFutureValue> {
 public:
  void AndThen(base::OnceCallback<void()> callback) {
    Future<VoidFutureValue>::AndThen(
        base::IgnoreArgs<VoidFutureValue>(std::move(callback)));
  }

  template <typename U>
  Future<U> AndThen(base::OnceCallback<Future<U>()> callback) {
    return Future<VoidFutureValue>::AndThen(
        base::IgnoreArgs<VoidFutureValue>(std::move(callback)));
  }

  template <typename U>
  Future<U> Transform(base::OnceCallback<U()> callback) {
    return Future<VoidFutureValue>::Transform(
        base::IgnoreArgs<VoidFutureValue>(std::move(callback)));
  }
};

template <>
class Promise<void> : public Promise<VoidFutureValue> {
 public:
  Future<void> GetFuture() {
    return Future<void>(Promise<VoidFutureValue>::GetFuture());
  }

  void SetValue() { Promise<VoidFutureValue>::SetValue({}); }

  void SetValueWithSideEffects() {
    Promise<VoidFutureValue>::SetValueWithSideEffects({});
  }
};

// Returns an already-completed future that wraps the provided value.
template <typename T>
Future<T> MakeReadyFuture(T value) {
  Promise<T> promise;
  promise.SetValue(std::move(value));
  return promise.GetFuture();
}

Future<void> MakeReadyFuture();

template <typename... Args>
struct PromiseExecutor {
  using Promise = Promise<std::tuple<Args...>>;

  template <typename F>
  static void Execute(F f, Promise promise) {
    f(base::BindOnce(
        [](Promise promise, Args... args) {
          promise.SetValue(std::make_tuple<Args...>(std::move(args)...));
        },
        std::move(promise)));
  }
};

template <typename T>
struct PromiseExecutor<T> {
  using Promise = Promise<T>;

  template <typename F>
  static void Execute(F f, Promise promise) {
    f(base::BindOnce(
        [](Promise promise, T value) { promise.SetValue(std::move(value)); },
        std::move(promise)));
  }
};

template <>
struct PromiseExecutor<void> {
  using Promise = Promise<void>;

  template <typename F>
  static void Execute(F f, Promise promise) {
    f(base::BindOnce([](Promise promise) { promise.SetValue(); },
                     std::move(promise)));
  }
};

template <>
struct PromiseExecutor<> : public PromiseExecutor<void> {};

template <typename... Args, typename F>
auto MakeFuture(F&& f) {
  using Executor = PromiseExecutor<Args...>;
  typename Executor::Promise promise;
  auto future = promise.GetFuture();
  Executor::Execute(std::forward<F>(f), std::move(promise));
  return future;
}

template <typename T>
class FutureAwaiter {
 public:
  explicit FutureAwaiter(Future<T> future) : future_(std::move(future)) {}

  bool await_ready() noexcept {
    value_ = future_.GetValueSynchronously();
    return value_.has_value();
  }

  void await_suspend(std::coroutine_handle<> handle) {
    // NOTE: Awaiter objects are alive until after `await_resume` or
    // `handle.destroy()` is called.
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
  std::optional<T> value_;
};

template <>
class FutureAwaiter<void> {
 public:
  explicit FutureAwaiter(Future<void> future) : future_(std::move(future)) {}

  bool await_ready() noexcept {
    return future_.GetValueSynchronously().has_value();
  }

  void await_suspend(std::coroutine_handle<> handle) {
    // NOTE: Awaiter objects are alive until after `await_resume` or
    // `handle.destroy()` is called.
    future_.AndThen(base::BindOnce(&FutureAwaiter::OnReady,
                                   base::Unretained(this), handle));
  }

  void await_resume() {}

 private:
  void OnReady(std::coroutine_handle<> handle) { handle.resume(); }

  Future<void> future_;
};

}  // namespace futures

// TODO(zenparsing): We should probably disallow any parameter args that are
// bare references or pointers.

// TODO(zenparsing): We should require a weak pointer when the coroutine is a
// member function.

template <typename T, typename... Args>
struct std::coroutine_traits<futures::Future<T>, Args...> {
  struct promise_type : public futures::Promise<T> {
    futures::Future<T> get_return_object() noexcept {
      return this->GetFuture();
    }

    std::suspend_never initial_suspend() const noexcept { return {}; }

    std::suspend_never final_suspend() const noexcept { return {}; }

    void return_value(T value) noexcept {
      this->SetValueWithSideEffects(std::move(value));
    }

    // TODO(zenparsing): Not sure if this overload is a good idea. Without it,
    // users will have to type `co_return co_await <expr>` frequently, but the
    // same shortcut cannot work for `Future<void>`.
    void return_value(futures::Future<T> future) noexcept {
      if (auto value = future.GetValueSynchronously()) {
        this->SetValueWithSideEffects(std::move(*value));
      } else {
        future.AndThen(base::BindOnce(
            [](futures::Promise<T> promise, T value) {
              promise.SetValueWithSideEffects(std::move(value));
            },
            std::move(*this)));
      }
    }

    void unhandled_exception() noexcept {}
  };
};

template <typename... Args>
struct std::coroutine_traits<futures::Future<void>, Args...> {
  struct promise_type : public futures::Promise<void> {
    futures::Future<void> get_return_object() noexcept {
      return this->GetFuture();
    }

    std::suspend_never initial_suspend() const noexcept { return {}; }

    std::suspend_never final_suspend() const noexcept { return {}; }

    void return_void() noexcept { this->SetValueWithSideEffects(); }

    void unhandled_exception() noexcept {}
  };
};

template <typename T>
auto operator co_await(futures::Future<T> future) {
  return futures::FutureAwaiter(std::move(future));
}

#endif  // BRAVE_COMPONENTS_FUTURES_FUTURE_H_
