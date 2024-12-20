// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_ASYNC_FUTURE_H_
#define BASE_ASYNC_FUTURE_H_

#include <concepts>
#include <coroutine>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"

namespace base {

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
  static_assert(!std::is_reference_v<T> && !std::is_pointer_v<T>,
                "Future values may not be references or pointers");

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
  void AndThen(OnceCallback<void(T)> callback) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (value_) {
      std::optional<T> value = std::move(value_);
      SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, BindOnce(std::move(callback), std::move(*value)));
    } else if (promise_ptr_) {
      promise_ptr_->SetCallback(std::move(callback));
      promise_ptr_ = nullptr;
    } else {
      NOTREACHED() << "Cannot attach a callback to an inactive future";
    }
  }

  // Attaches a transforming callback that will be executed when the future
  // value is available. Returns a future for the transformed value.
  template <typename U>
  Future<U> AndThen(OnceCallback<Future<U>(T)> callback) {
    Promise<U> promise;
    Future<U> future = promise.GetFuture();
    AndThen(BindOnce(TransformAndUnwrapFutureValue<U>, std::move(promise),
                     std::move(callback)));
    return future;
  }

  // Attaches a transforming callback that will be executed when the future
  // value is available. Returns a future for the transformed value.
  template <typename U>
  Future<U> Transform(OnceCallback<U(T)> callback) {
    Promise<U> promise;
    Future<U> future = promise.GetFuture();
    AndThen(BindOnce(TransformFutureValue<U>, std::move(promise),
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
                                   OnceCallback<U(T)> callback,
                                   T value) {
    promise.SetValue(std::move(callback).Run(std::move(value)));
  }

  template <typename U>
  static void TransformAndUnwrapFutureValue(Promise<U> promise,
                                            OnceCallback<Future<U>(T)> callback,
                                            T value) {
    std::move(callback)
        .Run(std::move(value))
        .AndThen(BindOnce(UnwrapFutureValue<U>, std::move(promise)));
  }

  template <typename U>
  static void UnwrapFutureValue(Promise<U> promise, U value) {
    promise.SetValue(std::move(value));
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
      value_set_ = other.value_set_;
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
  // registered for the associated future it will be executed synchronously. In
  // general, this method should only be used when the caller is known to be at
  // the "bottom" of the stack.
  void SetValueWithSideEffects(T value) { SetValue(std::move(value), true); }

 private:
  friend class Future<T>;

  void SetCallback(OnceCallback<void(T)> callback) {
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
        SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, BindOnce(std::move(callback_), std::move(value)));
      }
    } else if (future_ptr_) {
      future_ptr_->SetValue(std::move(value));
      future_ptr_ = nullptr;
    } else {
      CHECK(!value_set_) << "SetValue has already been called on this promise";
    }
    value_set_ = true;
  }

  SEQUENCE_CHECKER(sequence_checker_);
  std::optional<Future<T>> future_;
  raw_ptr<Future<T>> future_ptr_ = nullptr;
  OnceCallback<void(T)> callback_;
  bool value_set_ = false;
};

struct VoidFutureValue {};

template <>
class Future<void> : public Future<VoidFutureValue> {
 public:
  void AndThen(OnceCallback<void()> callback) {
    Future<VoidFutureValue>::AndThen(
        IgnoreArgs<VoidFutureValue>(std::move(callback)));
  }

  template <typename U>
  Future<U> AndThen(OnceCallback<Future<U>()> callback) {
    return Future<VoidFutureValue>::AndThen(
        IgnoreArgs<VoidFutureValue>(std::move(callback)));
  }

  template <typename U>
  Future<U> Transform(OnceCallback<U()> callback) {
    return Future<VoidFutureValue>::Transform(
        IgnoreArgs<VoidFutureValue>(std::move(callback)));
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

ALWAYS_INLINE Future<void> MakeReadyFuture() {
  Promise<void> promise;
  promise.SetValue();
  return promise.GetFuture();
}

namespace internal {

template <typename... Args>
struct MakeFutureHelper {
  using Promise = Promise<std::tuple<Args...>>;

  static auto WrapPromise(Promise promise) {
    return BindOnce(
        [](Promise promise, Args... args) {
          promise.SetValueWithSideEffects(
              std::make_tuple<Args...>(std::move(args)...));
        },
        std::move(promise));
  }
};

template <typename T>
struct MakeFutureHelper<T> {
  using Promise = Promise<T>;

  static auto WrapPromise(Promise promise) {
    return BindOnce(
        [](Promise promise, T value) {
          promise.SetValueWithSideEffects(std::move(value));
        },
        std::move(promise));
  }
};

template <>
struct MakeFutureHelper<void> {
  using Promise = Promise<void>;

  static auto WrapPromise(Promise promise) {
    return BindOnce([](Promise promise) { promise.SetValueWithSideEffects(); },
                    std::move(promise));
  }
};

template <>
struct MakeFutureHelper<> : public MakeFutureHelper<void> {};

}  // namespace internal

// Creates a promise/future pair, and calls the specified function with a
// callback of type `OnceCallback<void(Args...)>`. The Future value type depends
// upon the number of type arguments supplied, as follows:
//
// - None: `Future<void>`
// - One: `Future<T>`
// - More than one: `Future<std::tuple<Args...>>`
//
// When run, the callback function will set the value of the corresponding
// promise object. It may be called from any sequence.
template <typename... Args, typename F>
auto MakeFuture(F&& f) {
  using MakeFutureHelper = internal::MakeFutureHelper<Args...>;
  typename MakeFutureHelper::Promise promise;
  auto future = promise.GetFuture();
  auto callback = MakeFutureHelper::WrapPromise(std::move(promise));
  f(BindPostTaskToCurrentDefault(std::move(callback)));
  return future;
}

// Coroutine Support

namespace internal {

template <typename T>
concept MaybeResumable = requires(T promise) {
  { promise.CanResume() } -> std::same_as<bool>;
};

template <typename T>
class FutureAwaiter {
 public:
  explicit FutureAwaiter(Future<T> future) : future_(std::move(future)) {}

  bool await_ready() noexcept {
    value_ = future_.GetValueSynchronously();
    return value_.has_value();
  }

  template <typename P>
  void await_suspend(std::coroutine_handle<P> handle) noexcept {
    future_.AndThen(
        BindOnce(&FutureAwaiter::OnReady<P>, Unretained(this), handle));
  }

  T await_resume() noexcept { return std::move(*value_); }

 private:
  template <typename P>
  void OnReady(std::coroutine_handle<P> handle, T value) {
    if constexpr (MaybeResumable<P>) {
      if (!handle.promise().CanResume()) {
        handle.destroy();
        return;
      }
    }
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

  template <typename P>
  void await_suspend(std::coroutine_handle<P> handle) noexcept {
    future_.AndThen(
        BindOnce(&FutureAwaiter::OnReady<P>, Unretained(this), handle));
  }

  void await_resume() noexcept {}

 private:
  template <typename P>
  void OnReady(std::coroutine_handle<P> handle) {
    if constexpr (MaybeResumable<P>) {
      if (!handle.promise().CanResume()) {
        handle.destroy();
        return;
      }
    }
    handle.resume();
  }

  Future<void> future_;
};

template <typename T>
class CoroutinePromiseBase : public Promise<T> {
 public:
  Future<T> get_return_object() noexcept { return this->GetFuture(); }
  std::suspend_never initial_suspend() const noexcept { return {}; }
  std::suspend_never final_suspend() const noexcept { return {}; }
  void return_value(T value) noexcept { this->SetValue(std::move(value)); }

  void return_value(Future<T> future) noexcept {
    future.AndThen(BindOnce(
        [](Promise<T> promise, T value) { promise.SetValue(std::move(value)); },
        std::move(*this)));
  }

  void unhandled_exception() noexcept {}
};

template <>
class CoroutinePromiseBase<void> : Promise<void> {
 public:
  Future<void> get_return_object() noexcept { return this->GetFuture(); }
  std::suspend_never initial_suspend() const noexcept { return {}; }
  std::suspend_never final_suspend() const noexcept { return {}; }
  void return_void() noexcept { this->SetValue(); }
  void unhandled_exception() noexcept {}
};

struct CoroutineArgPlaceholder {
  explicit operator bool() const { return true; }
};

template <typename T>
struct CoroutineEmptyArgTraits {
  using WeakPtrType = CoroutineArgPlaceholder;

  static WeakPtrType GetPlaceholder() {
    static_assert(std::is_empty_v<T>,
                  "Capturing lambdas cannot be used as coroutines.");
    return {};
  }

  static WeakPtrType GetWeakPtr(T&) {
    static_assert(std::is_empty_v<T>,
                  "Non-empty coroutine arguments passed by reference ("
                  "including the implicit this pointer for member functions) "
                  "must implement AsWeakPtr().");
    return {};
  }

  static WeakPtrType GetWeakPtr(T* obj) { return GetWeakPtr(*obj); }
};

template <typename T>
struct CoroutineRefArgTraits {
  using WeakPtrType = WeakPtr<T>;
  static WeakPtrType GetWeakPtr(T& obj) { return obj.AsWeakPtr(); }
  static WeakPtrType GetWeakPtr(T* obj) { return obj->AsWeakPtr(); }
};

template <typename T>
struct CoroutineArgTraits {
  using WeakPtrType = CoroutineArgPlaceholder;
  static WeakPtrType GetWeakPtr(T&) { return {}; }
};

template <typename T>
struct CoroutineArgTraits<T&> : public CoroutineEmptyArgTraits<T> {};

template <typename T>
struct CoroutineArgTraits<T*> : public CoroutineEmptyArgTraits<T> {};

template <typename T>
concept ProvidesWeakPtr = requires(T obj) {
  { obj.AsWeakPtr() } -> std::same_as<WeakPtr<T>>;
};

template <typename T>
  requires(ProvidesWeakPtr<T>)
struct CoroutineArgTraits<T&> : public CoroutineRefArgTraits<T> {};

template <typename T>
  requires(ProvidesWeakPtr<T>)
struct CoroutineArgTraits<T*> : public CoroutineRefArgTraits<T> {};

template <typename T>
using CoroutineArgWeakPtrType = CoroutineArgTraits<T>::WeakPtrType;

template <typename Head, typename... Tail>
struct CoroutinePromiseConstructorHelper {
  template <typename... ConstructorArgs>
  static auto MakeTuple(ConstructorArgs&... args) {
    return std::make_tuple(CoroutineArgTraits<Head>::GetPlaceholder(),
                           CoroutineArgTraits<Tail>::GetWeakPtr(args)...);
  }
};

template <typename T, typename... Args>
class CoroutinePromise : public CoroutinePromiseBase<T> {
 public:
  explicit CoroutinePromise(Args&... args)
      : ptrs_(CoroutineArgTraits<Args>::GetWeakPtr(args)...) {}

  template <typename... ConstructorArgs>
    requires(sizeof...(Args) == sizeof...(ConstructorArgs) + 1)
  explicit CoroutinePromise(ConstructorArgs&... args)
      : ptrs_(CoroutinePromiseConstructorHelper<Args...>::MakeTuple(args...)) {}

  bool CanResume() const {
    return std::apply([](auto&&... args) { return (!!args && ...); }, ptrs_);
  }

 private:
  std::tuple<CoroutineArgWeakPtrType<Args>...> ptrs_;
};

}  // namespace internal

}  // namespace base

template <typename T, typename... Args>
struct std::coroutine_traits<base::Future<T>, Args...> {
  using promise_type = base::internal::CoroutinePromise<T, Args...>;
};

template <typename T>
auto operator co_await(base::Future<T> future) {
  return base::internal::FutureAwaiter(std::move(future));
}

#endif  // BASE_ASYNC_FUTURE_H_
