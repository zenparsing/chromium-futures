/* Copyright (c) 2022 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_COMPONENTS_FUTURES_SHARED_PROMISE_H_
#define BRAVE_COMPONENTS_FUTURES_SHARED_PROMISE_H_

#include <list>
#include <utility>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/sequence_checker.h"
#include "brave/components/futures/future.h"

namespace futures {

// Like Promise<T>, except that it is is copyable and can safely be used from
// a different sequence.
//
// Example:
//   Promise<int> promise;
//   Future<int> future = promise.GetFuture();
//   SharedPromise<int> shared_promise(std::move(promise));
//   task_runner->PostTask(
//       FROM_HERE,
//       base::BindOnce(DoWorkInOtherSequence, std::move(shared_promise)));
//   future.AndThen(base::BindOnce([](int value) {}));
template <typename T>
class SharedPromise {
 public:
  explicit SharedPromise(Promise<T> promise)
      : state_(new State(std::move(promise))),
        task_runner_(base::SequencedTaskRunnerHandle::Get()) {}

  // Sets the value of the underlying promise.
  void SetValue(T value) {
    task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&State::SetValue, state_, std::move(value)));
  }

 private:
  class State : public base::RefCountedThreadSafe<State> {
   public:
    explicit State(Promise<T> promise) : promise_(std::move(promise)) {}

    void SetValue(T value) {
      DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
      if (promise_) {
        promise_->SetValue(std::move(value));
        promise_.reset();
      }
    }

   private:
    friend class base::RefCountedThreadSafe<State>;
    ~State() = default;

    absl::optional<Promise<T>> promise_;
    SEQUENCE_CHECKER(sequence_checker_);
  };

  scoped_refptr<State> state_;
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
};

}  // namespace futures

#endif  // BRAVE_COMPONENTS_FUTURES_SHARED_PROMISE_H_
