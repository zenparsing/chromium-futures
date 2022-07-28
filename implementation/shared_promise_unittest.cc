/* Copyright (c) 2022 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/futures/shared_promise.h"

#include "base/run_loop.h"
#include "base/task/thread_pool.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace futures {

class SharedPromiseTest : public testing::Test {
 protected:
  base::test::TaskEnvironment task_environment_;
};

TEST_F(SharedPromiseTest, SharedPromiseOnDifferentThread) {
  int value = 0;
  auto set_value = [&value](int v) { value = v; };

  Promise<int> promise;
  promise.GetFuture().AndThen(base::BindLambdaForTesting(set_value));
  SharedPromise<int> shared_promise(std::move(promise));

  auto task_runner = base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN});

  auto thread_worker = [](base::RunLoop* run_loop, SharedPromise<int> p) {
    p.SetValue(42);
    run_loop->Quit();
  };

  base::RunLoop run_loop;
  task_runner->PostTask(FROM_HERE, base::BindOnce(thread_worker, &run_loop,
                                                  std::move(shared_promise)));
  run_loop.Run();

  task_environment_.RunUntilIdle();
  EXPECT_EQ(value, 42);
}

TEST_F(SharedPromiseTest, FirstValueWins) {
  int value = 0;
  int calls = 0;
  auto set_value = [&value, &calls](int v) {
    value = v;
    ++calls;
  };

  Promise<int> promise;
  promise.GetFuture().AndThen(base::BindLambdaForTesting(set_value));
  SharedPromise<int> shared_promise1(std::move(promise));
  SharedPromise<int> shared_promise2(shared_promise1);

  shared_promise1.SetValue(42);
  shared_promise2.SetValue(24);

  task_environment_.RunUntilIdle();

  EXPECT_EQ(value, 42);
  EXPECT_EQ(calls, 1);
}

}  // namespace futures
