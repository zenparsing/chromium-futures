template <typename T>
class FutureAwaiter {
 public:
  explicit FutureAwaiter(Future<T> future) : future_(std::move(future)) {}

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> resume) {
    future_.Then(base::BindOnce(&FutureAwaiter::OnReady,
                 weak_factory_.GetWeakPtr(), resume));
  }

  T await_resume() { return std::move(*value_); }

 private:
  void OnReady(std::coroutine_handle<> resume, T value) {
    value_ = std::move(value);
    resume();
  }

  Future<T> future_;
  absl::optional<T> value_;
  base::WeakFactory<FutureAwaiter> weak_factory_{this};
};

template <typename T, typename... Args>
struct std::coroutine_traits<Future<T>, Args...> {
  struct promise_type : public Promise<T> {
    Future<T> get_return_object() { return GetFuture(); }

    std::suspend_never initial_suspend() const { return {}; }

    std::suspend_never final_suspend() const { return {}; }

    void return_value(T value) { SetValue(std::move(value)); }

    void return_value(Future<T> future) {
      future.Then(base::BindOnce([](Promise<T> promise, T value) {
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
