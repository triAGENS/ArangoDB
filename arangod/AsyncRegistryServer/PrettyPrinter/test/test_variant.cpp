#include <cstddef>
#include <variant>
#include <iostream>
#include <fstream>
#include <atomic>

using PromiseId = void*;
struct ThreadId {
  size_t some_id;
};

struct Requester : std::variant<ThreadId, PromiseId, size_t> {};

struct Promise {
  auto id() -> void* { return this; }

  std::atomic<Requester> requester;
  Promise* next;
};

struct PromiseList {
  std::atomic<Promise*> head;
};

int main() {
  auto sync_promise = Promise{Requester{ThreadId{5}}, nullptr};
  auto async_promise = Promise{Requester{&sync_promise}, &sync_promise};
  auto list = PromiseList{&async_promise};

  std::ofstream myfile;
  myfile.open("example.txt");
  myfile << "sync: " << sync_promise.id() << ", async: " << async_promise.id()
         << std::endl;
  myfile.close();

  std::cout << list.head.load()->id() << std::endl;
}
