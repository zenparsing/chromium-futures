# FAQ

## Why "Future" and "Promise"? Aren't those name taken/poisoned?

There aren't many good options for naming an object that represents the return
value of an asynchronous C++ function:

* `Task`/`TaskResolver`: Already used for the task scheduling system in Chromium.
Not an option.
* `Future`/`Promise`: Conflicts subtley with `std::future` and `std::promise`.
Also conflicts with `base::test::TestFuture`. Also, `x11::Future` is a thing.
* `Promise`/`PromiseResolver`: A JS-inspired option. It would be awkward for the
thing that C++ calls a "promise" to be called here a "promise resolver", and
for the thing that C++ calls a "future" to be called here a "promise".
* `Async`/`AsyncResolver`: Awkward in usage because it is not a noun. Although
"async something" is communicative, how does one talk about an async thing in
general? What would we call a collection of async things? "asyncs"? Also,
conflicts with `std::async`.
* `Co`: Awkward because it means nothing. Also, it is overly general since
C++ coroutines can be used for many different applications depending on the
return type.
* `Awaitable`/`AwaitableResolver`: Overly long. Also, it is overly general since
many things may be `co_await`-able depending on how `operator co_await` is
overloaded.

In the author's opinion, since there are no clear winners, "Future"/"Promise" is
the least-bad option.

### Regarding std::promise and std::future

A conflict with a type in `std` is unfortunate. On the other hand, in C++ the
"promise" term is already overloaded:

* `std::promise`
* `std::coroutine_traits::promise_type`

which suggests that "promise" is a general term that may be applied to a
variety of types. Furthermore, the proposed `base::Promise` API is
intentionally similar in shape to `std::promise`:

* `std::promise::set_value` => `base::Promise::SetValue`
* `std::promise::get_future` => `base::Promise::GetFuture`

It should also be noted that Chromium is already using the "future" name for
something that is quite unlike `std::future`.

### Regarding base::test::TestFuture

The `TestFuture` API is quite ergonomic for writing tests that must wait on an
async value. Clearly there is a need for a future-like API. If this
`base::Future` type is added, then `TestFuture` should be deprecated and all
usage should be migrated to using `base::Future`, along with a test-only
function that will block until a future is resolved:

```cpp

namespace base::test {

template <typename T>
T WaitFor(Future<T> future) {
  std::optional<T> value;
  RunLoop loop;
  std::move(future).AndThen(base::BindLambdaForTesting([&](T val) {
    value = std::move(val);
    loop.Quit();
  }));
  loop.Run();
  return *value;
}

}  // namespace base::test

```

### Regarding x11::Future

`x11::Future` is a typical future type that allows attaching a completion
callback using the `OnResponse` method. The future value type is `Reply<V, E>`
which is functionally similar to `expected<V, E>`. In addition to asynchronously
listening for a completion value, `x11:Future` also allows the holder to stop
the current thread's execution until the server response has arrived. Since
`base::Future` is not intended for blocking synchronization, how could this
design be altered to allow integration with `co_await`?

We can note that if the caller wants to wait for the response, then there's no
need for either a future or a `co_await`. It can just call `Sync()` and get the
value directly. It might make sense therefore to refactor the code so that
`x11::Future` becomes something like `x11::PendingResponse` (or `x11::Response`
if some other name changes can be made) and in addition to the `OnResponse`
method, it would expose a `GetFuture` method that would return a `base::Future`.

## Weren't Promises rejected for Chromium a while ago?

Yes. There was a effort from several years ago which attempted to add a
JavaScript-like Promise API to Chromium. In the author's opinion, that proposal
was extremely complex and did not fit well with the either the task scheduling
library or C++ in general.

## Why doesn't Future accept multiple type parameters?

In order for smooth iteroperability with `co_await`, a `Future` is a strict
representation ("reification") of the result of a C++ function call. A C++
function call cannot return multiple values.

## Why doesn't Future support errors?

Since Chromium does not support exceptions, a function cannot "throw" an
error. Since a `Future` is strictly a representation of the result of a C++
function call, it does not need to support an error return "channel". Users
that want to provide an error value can do so using `expected<V, E>`.
