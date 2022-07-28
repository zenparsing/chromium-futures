/* Copyright (c) 2022 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/futures/future.h"

#include <string>
#include <tuple>

#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace futures {

class FutureTest : public testing::Test {
 protected:
  base::test::TaskEnvironment task_environment_;
};

TEST_F(FutureTest, ValueSentInFutureTurn) {
  int value = 0;
  MakeReadyFuture(10).AndThen(
      base::BindLambdaForTesting([&value](int v) { value = v; }));
  EXPECT_EQ(value, 0);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(value, 10);
}

TEST_F(FutureTest, CompleteCallbacksExecutedInFutureTurn) {
  Promise<int> promise;
  int value = 0;
  promise.GetFuture().AndThen(
      base::BindLambdaForTesting([&value](int v) { value = v; }));
  promise.SetValue(1);
  EXPECT_EQ(value, 0);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(value, 1);
}

TEST_F(FutureTest, Transform) {
  double value = 0;

  MakeReadyFuture(1)
      .Transform(
          base::BindOnce([](int v) { return static_cast<double>(v) / 2; }))
      .AndThen(base::BindLambdaForTesting([&value](double v) { value = v; }));

  EXPECT_EQ(value, 0);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(value, 0.5);
}

TEST_F(FutureTest, AndThen) {
  bool value = false;

  MakeReadyFuture(42)
      .AndThen(base::BindOnce([](int value) { return MakeReadyFuture(true); }))
      .AndThen(base::BindLambdaForTesting([&value](bool v) { value = v; }));

  EXPECT_FALSE(value);
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(value);
}

TEST_F(FutureTest, GetValueSynchronously) {
  auto value = MakeReadyFuture(1).GetValueSynchronously();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 1);
}

TEST_F(FutureTest, MakeReadyFuture) {
  int value = 0;
  MakeReadyFuture(1).AndThen(
      base::BindLambdaForTesting([&value](int v) { value = v; }));
  EXPECT_EQ(value, 0);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(value, 1);
}

TEST_F(FutureTest, MakeFuture1) {
  int value = 0;

  MakeFuture<int>([&](auto resolve) {
    std::move(resolve).Run(42);
  }).AndThen(base::BindLambdaForTesting([&](int v) { value = v; }));

  task_environment_.RunUntilIdle();
  EXPECT_EQ(value, 42);
}

TEST_F(FutureTest, MakeFuture0) {
  bool called = false;

  MakeFuture([&](auto resolve) {
    std::move(resolve).Run();
  }).AndThen(base::BindLambdaForTesting([&](std::tuple<>) { called = true; }));

  task_environment_.RunUntilIdle();
  EXPECT_TRUE(called);
}

TEST_F(FutureTest, MakeFutureN) {
  std::tuple<int, bool, double> values = {0, false, 0.0};

  MakeFuture<int, bool, double>([&](auto resolve) {
    std::move(resolve).Run(42, true, 1.3);
  }).AndThen(base::BindLambdaForTesting([&](std::tuple<int, bool, double> v) {
    values = v;
  }));

  task_environment_.RunUntilIdle();
  EXPECT_EQ(std::get<0>(values), 42);
  EXPECT_TRUE(std::get<1>(values));
  EXPECT_EQ(std::get<2>(values), 1.3);
}

}  // namespace futures
