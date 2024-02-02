/* Copyright (c) 2023 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_COMPONENTS_FUTURES_SHARED_FUTURE_H_
#define BRAVE_COMPONENTS_FUTURES_SHARED_FUTURE_H_

#include <list>
#include <optional>
#include <utility>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/sequence_checker.h"
#include "base/task/single_thread_task_runner.h"
#include "brave/components/futures/future.h"

namespace futures {

// Like Future<T>, except that it is is copyable and thread-safe, and the result
// of the asynchronous operation is revealed to continuations as a const
// reference. Always prefer |Future| to |SharedFuture|. In general,
// |SharedFuture| should only be used when the result needs to be cached or
// deduped.
//
// Example:
//   Future<int> future = MakeReadyFuture(42);
//   SharedFuture shared_future(std::move(future));
//   shared_future.AndThen(base::BindOnce([](const int& value) {}));
template <typename T>
class SharedFuture {
 public:
  using ValueType = T;

  explicit SharedFuture(Future<T> future)
      : state_(new State()),
        task_runner_(base::SequencedTaskRunner::GetCurrentDefault()) {
    future.AndThen(base::BindOnce(OnComplete, state_, task_runner_));
  }

  SharedFuture(const SharedFuture& other) = default;
  SharedFuture& operator=(const SharedFuture& other) = default;

  SharedFuture(SharedFuture&& other) = default;
  SharedFuture& operator=(SharedFuture&& other) = default;

  // Attaches a callback that will be executed when the shared future value is
  // available. The callback will be executed on the caller's task runner.
  void AndThen(base::OnceCallback<void(const T&)> callback) {
    Listener listener{
        .on_complete = std::move(callback),
        .task_runner = base::SequencedTaskRunner::GetCurrentDefault()};
    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&State::AddListener, state_, std::move(listener)));
  }

  // Attaches a transforming callback that will be executed when the shared
  // future value is available. Returns a non-shared future for the transformed
  // value.
  template <typename U>
  Future<U> AndThen(base::OnceCallback<Future<U>(const T&)> callback) {
    Promise<U> promise;
    Future<U> future = promise.GetFuture();
    Then(base::BindOnce(TransformAndUnwrapFutureValue<U>, std::move(promise),
                        std::move(callback)));
    return future;
  }

  // Attaches a transforming callback that will be executed when the shared
  // future value is available. Returns a non-shared future for the transformed
  // value.
  template <typename U>
  Future<U> Transform(base::OnceCallback<U(const T&)> callback) {
    Promise<U> promise;
    Future<U> future = promise.GetFuture();
    Then(base::BindOnce(TransformFutureValue<U>, std::move(promise),
                        std::move(callback)));
    return future;
  }

 private:
  class State;

  static void OnComplete(scoped_refptr<State> state,
                         scoped_refptr<base::SequencedTaskRunner> task_runner,
                         T value) {
    task_runner->PostTask(
        FROM_HERE, base::BindOnce(&State::SetValue, state, std::move(value)));
  }

  struct Listener {
    base::OnceCallback<void(const T&)> on_complete;
    scoped_refptr<base::SequencedTaskRunner> task_runner;
  };

  class State : public base::RefCountedThreadSafe<State> {
   public:
    void SetValue(T value) {
      DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
      if (value_) {
        return;
      }

      value_ = std::move(value);

      for (auto& listener : listeners_) {
        listener.task_runner->PostTask(
            FROM_HERE,
            base::BindOnce(&State::RunCompleteCallback, this,
                           std::move(listener.on_complete), *value_));
      }

      listeners_.clear();
    }

    void AddListener(Listener listener) {
      DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
      if (value_) {
        listener.task_runner->PostTask(
            FROM_HERE,
            base::BindOnce(&State::RunCompleteCallback, this,
                           std::move(listener.on_complete), *value_));
      } else {
        listeners_.push_back(std::move(listener));
      }
    }

   private:
    friend class base::RefCountedThreadSafe<State>;
    ~State() = default;

    void RunCompleteCallback(base::OnceCallback<void(const T&)> callback,
                             const T& value) {
      std::move(callback).Run(value);
    }

    std::optional<T> value_;
    std::list<Listener> listeners_;
    SEQUENCE_CHECKER(sequence_checker_);
  };

  template <typename U>
  static void TransformFutureValue(Promise<U> promise,
                                   base::OnceCallback<U(const T&)> callback,
                                   const T& value) {
    promise.SetValueWithSideEffects(std::move(callback).Run(value));
  }

  template <typename U>
  static void TransformAndUnwrapFutureValue(
      Promise<U> promise,
      base::OnceCallback<Future<U>(const T&)> callback,
      const T& value) {
    std::move(callback).Run(value).Then(
        base::BindOnce(UnwrapFutureValue<U>, std::move(promise)));
  }

  template <typename U>
  static void UnwrapFutureValue(Promise<U> promise, U value) {
    promise.SetValueWithSideEffects(std::move(value));
  }

  scoped_refptr<State> state_;
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
};

template <>
class SharedFuture<void> : public SharedFuture<VoidFutureValue> {
 public:
  explicit SharedFuture(Future<void> future)
      : SharedFuture<VoidFutureValue>(std::move(future)) {}

  void AndThen(base::OnceCallback<void()> callback) {
    SharedFuture<VoidFutureValue>::AndThen(
        base::IgnoreArgs<const VoidFutureValue&>(std::move(callback)));
  }

  template <typename U>
  Future<U> AndThen(base::OnceCallback<Future<U>()> callback) {
    return SharedFuture<VoidFutureValue>::AndThen(
        base::IgnoreArgs<const VoidFutureValue&>(std::move(callback)));
  }

  template <typename U>
  Future<U> Transform(base::OnceCallback<U()> callback) {
    return SharedFuture<VoidFutureValue>::Transform(
        base::IgnoreArgs<const VoidFutureValue&>(std::move(callback)));
  }
};

}  // namespace futures

#endif  // BRAVE_COMPONENTS_FUTURES_SHARED_FUTURE_H_
