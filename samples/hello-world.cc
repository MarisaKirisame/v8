// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <boost/math/statistics/anderson_darling.hpp>
#include <nlohmann/json.hpp>
#include "include/libplatform/libplatform.h"
#include "include/v8.h"
#include <chrono>
#include <random>
#include <future>
#include <thread>

using namespace nlohmann;
using boost::accumulators::accumulator_set;
namespace tag = boost::accumulators::tag;
using boost::accumulators::stats;
using time_point = std::chrono::steady_clock::time_point;
using std::chrono::steady_clock;
using std::chrono::duration_cast;
using milliseconds = std::chrono::milliseconds;
double mean(const std::vector<double>& v) {
  accumulator_set<double, stats<tag::variance>> acc;
  for (double d: v) {
    acc(d);
  }
  return boost::accumulators::extract::mean(acc);
}

double sd(const std::vector<double>& v) {
  accumulator_set<double, stats<tag::variance>> acc;
  for (double d: v) {
    acc(d);
  }
  return sqrt(boost::accumulators::extract::variance(acc));
}

double normality(const std::vector<double>& v) {
  return boost::math::statistics::anderson_darling_normality_statistic(v);
}

v8::Local<v8::String> fromFile(v8::Isolate* isolate, const std::string& path) {
  std::ifstream t(path);
  std::string str((std::istreambuf_iterator<char>(t)),
                  std::istreambuf_iterator<char>());
  return v8::String::NewFromUtf8(isolate, str.data()).ToLocalChecked();
}

constexpr size_t min_heap_size = 2359296;
constexpr size_t max_heap_size = 4e9; // Beyond 4gb ConfigureDefaults start acting funny.

size_t random_heap_size() {
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_real_distribution<> dist(log(min_heap_size), log(max_heap_size));
  return exp(dist(rd));
}

struct Input {
  size_t heap_size;
  std::string code_path;
};

// I'd love to use the name from_json and to_json, but unfortunately it seems like the two name is used.
Input read_from_json(const json& j) {
  assert(j.count("heap_size") == 1);
  assert(j.count("code_path") == 1);
  size_t heap_size = j.value("heap_size", 0);
  std::string code_path = j.value("code_path", "");
  return Input {heap_size, code_path};
}

struct Output {
  std::string version;
  size_t time_taken;
};

void add_to_json(const Output& o, json& j) {
  j["version"] = o.version;
  j["time_taken"] = o.time_taken;
}

Output run(const Input& i, std::mutex* m) {
  std::cout << "running " << i.code_path << std::endl;
  Output o;
  // Create a new Isolate and make it the current one.
  v8::Isolate::CreateParams create_params;
  //create_params.constraints.ConfigureDefaults(heap_size, 0);
  create_params.constraints.ConfigureDefaultsFromHeapSize(i.heap_size, i.heap_size);
  size_t old = create_params.constraints.max_old_generation_size_in_bytes();
  size_t young = create_params.constraints.max_young_generation_size_in_bytes();
  //std::cout << old << " " << young << " " << old + young << std::endl;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  isolate->SetMaxPhysicalMemoryOfDevice(1e9);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    // Create a stack-allocated handle scope.
    v8::HandleScope handle_scope(isolate);
    // Create a new context.
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    // Enter the context for compiling and running the hello world script.
    v8::Context::Scope context_scope(context);

    {
      // Create a string containing the JavaScript source code.
      v8::Local<v8::String> source = fromFile(isolate, i.code_path);

      // Compile the source code.
      v8::Local<v8::Script> script =
          v8::Script::Compile(context, source).ToLocalChecked();

      m->lock();
      m->unlock(); // abusing mutex as signal - once the mutex is unlocked everyone get access.
      time_point begin = steady_clock::now();
      v8::Local<v8::Value> result;
      result = script->Run(context).ToLocalChecked();
      time_point end = steady_clock::now();
      o.time_taken = duration_cast<milliseconds>(end - begin).count();
      // Convert the result to an UTF8 string and print it.
      v8::String::Utf8Value utf8(isolate, result);
      printf("%s\n", *utf8);
    }
    v8::GCHistory history = isolate->GetGCHistory();
    for (const v8::GCRecord& r: history.records) {
      if (false && r.is_major_gc) {
        std::cout << "gc decrease memory by: " << long(r.before_memory) - long(r.after_memory) <<
          " in: " << r.after_time - r.before_time <<
          " rate: " << (long(r.before_memory) - long(r.after_memory)) / (r.after_time - r.before_time) <<
          " (is " << (r.is_major_gc ? std::string("major ") : std::string("minor ")) << "GC)" << std::endl;
      }
    }
    long total_garbage_collected = 0;
    long total_time_taken = 0;
    std::vector<double> garbage_collected, time_taken;
    for (const v8::GCRecord& r: history.records) {
      if (r.is_major_gc) {
        total_garbage_collected += long(r.before_memory) - long(r.after_memory);
        garbage_collected.push_back(long(r.before_memory) - long(r.after_memory));
        total_time_taken += r.after_time - r.before_time;
        time_taken.push_back(r.after_time - r.before_time);
      }
    }
    std::sort(garbage_collected.begin(), garbage_collected.end());
    std::sort(time_taken.begin(), time_taken.end());
    //std::cout << "total garbage collected: " << total_garbage_collected << std::endl;
    //double mean_garbage_collected = mean(garbage_collected);
    //double sd_garbage_collected = sd(garbage_collected);
    //double normality_garbage_collected = normality(garbage_collected);
    //std::cout << "mean, sd, normality of garbage collected: " << mean_garbage_collected << ", " << sd_garbage_collected << ", " << normality_garbage_collected << std::endl;
    //std::cout << "total time taken: " << total_time_taken << std::endl;
    //double mean_time_taken = mean(time_taken);
    //double sd_time_taken = sd(time_taken);
    //double normality_time_taken = normality(time_taken);
    //std::cout << "mean, sd, normality of time taken: " << mean_time_taken << ", " << sd_time_taken << ", " << normality_time_taken << std::endl;
    //std::cout << "garbage collection rate: " << double(total_garbage_collected) / double(total_time_taken) << std::endl;
  }
  isolate->Dispose();
  delete create_params.array_buffer_allocator;
  o.version = "2020-11-20";
  return o;
}

std::string get_time() {
  std::time_t t = std::time(nullptr);
  std::tm* tm = std::localtime(&t);
  return
    std::to_string(1900+tm->tm_year) + "-" +
    std::to_string(1+tm->tm_mon) + "-" +
    std::to_string(tm->tm_mday) + "-" +
    std::to_string(tm->tm_hour) + "-" +
    std::to_string(tm->tm_min) + "-" +
    std::to_string(tm->tm_sec);
}

void log_json(const json& j) {
  std::ofstream f("logs/" + get_time());
  f << j;
}

void read_write() {
  std::mutex m;
  m.lock();

  std::ifstream t("balancer-config");
  json j;
  t >> j;

  Input i = read_from_json(j);
  std::future<Output> o = std::async(std::launch::async, run, i, &m);

  m.unlock();
  add_to_json(o.get(), j);

  log_json(j);
}

double memory_score(size_t working_memory, size_t max_memory, double garbage_rate, size_t gc_time) {
  size_t extra_memory = max_memory - working_memory;
  return extra_memory / garbage_rate * extra_memory / gc_time;
}

struct RuntimeNode {
  virtual ~RuntimeNode() {}
  virtual size_t working_memory() = 0;
  virtual size_t max_memory() = 0;
  virtual double garbage_rate() = 0;
  virtual size_t gc_time() = 0; // we assume each gc clear all garbage: for generational gc only full gc 'count'
  virtual void allow_more_memory(size_t extra) = 0;
  virtual void shrink_max_memory() = 0;
  double memory_score() {
    return ::memory_score(working_memory(), max_memory(), garbage_rate(), gc_time());
  }
};
using Runtime = std::shared_ptr<RuntimeNode>;

struct Controller;
struct SimulatedRuntimeNode : RuntimeNode {
  size_t working_memory_;
  size_t working_memory() override {
    return working_memory_;
  }
  size_t max_memory_;
  size_t max_memory() override {
    return max_memory_;
  }
  size_t garbage_rate_;
  double garbage_rate() override {
    return garbage_rate_;
  }
  size_t gc_time_;
  size_t gc_time() override {
    return gc_time_;
  }
  void allow_more_memory(size_t extra) override {
    max_memory_ += extra;
  }
  void shrink_max_memory() override {
    max_memory_ = working_memory_;
  }
  bool in_gc = false;
  size_t time_in_gc = 0;
  size_t work_done = 0;
  bool need_gc() {
    return working_memory_ + garbage_rate_ <= max_memory_;
  }
  void mutator_tick() {
    ++work_done;
    working_memory_ += garbage_rate_;
  }
  void tick();
  SimulatedRuntimeNode(size_t working_memory_, size_t garbage_rate_, size_t gc_time_, Controller* c) :
    working_memory_(working_memory_), garbage_rate_(garbage_rate_), gc_time_(gc_time_), c(c) {
  }
  Controller* c;
};

struct Controller {
  size_t max_memory;
  size_t used_memory;
  double balance_factor;
  std::vector<Runtime> runtimes;
  Controller(size_t max_memory) : max_memory(max_memory), used_memory(0) {
    
  }
  bool request(RuntimeNode* r, size_t extra) {
    if (max_memory - used_memory >= extra) {
      used_memory += extra;
      r->allow_more_memory(extra);
      return true;
    } else {
      return false;
    }
  }
  void optimize() {
    
  }
};

void SimulatedRuntimeNode::tick() {
    if (in_gc) {
      ++time_in_gc;
      if (time_in_gc == gc_time_) {
        in_gc = false;
        time_in_gc = 0;
        max_memory_ = working_memory_;
      }
    } else if (need_gc()) {
      c->request(this, garbage_rate_);
      if (need_gc()) {
        in_gc = true;
        tick();
      } else {
        mutator_tick();
      }
    } else {
      mutator_tick();
    }
}
void parallel_experiment() {
  std::mutex m;
  m.lock();

  Input splay_input;
  splay_input.heap_size = 0;//300*1e6;
  splay_input.code_path = "splay.js";

  Input pdfjs_input;
  pdfjs_input.heap_size = 0;//700*1e6;
  pdfjs_input.code_path = "pdfjs.js";

  std::vector<Input> inputs;
  for (int i = 0; i < 2; ++i) {
    inputs.push_back(splay_input);
    inputs.push_back(pdfjs_input);
  }

  std::vector<std::future<Output>> futures;
  for (const Input& input : inputs) {
    futures.push_back(std::async(std::launch::async, run, input, &m));
  }

  m.unlock();

  size_t total_time = 0;
  for (std::future<Output>& future : futures) {
    total_time += future.get().time_taken;
  }

  std::cout << "total_time = " << total_time << std::endl;
}

void simulated_experiment() {
  Controller c(100);
  std::vector<std::shared_ptr<SimulatedRuntimeNode>> runtimes;
  runtimes.push_back(std::make_shared<SimulatedRuntimeNode>(1, 1, 1, &c));
  for (const auto& r: runtimes) {
    c.runtimes.push_back(r);
  }
  for (size_t i = 0; i < 10000; ++i) {
    for (const auto&r : runtimes) {
      r->tick();
    }
    if (i % 100 == 0) {
      c.optimize();
    }
  }
}

struct RestrictedPlatform : v8::Platform {
  RestrictedPlatform(std::unique_ptr<v8::Platform> &&platform_) : platform_(std::move(platform_)) { }
  std::unique_ptr<v8::Platform> platform_;
  v8::PageAllocator* GetPageAllocator() override {
    return platform_->GetPageAllocator();
  }

  void OnCriticalMemoryPressure() override {
    return platform_->OnCriticalMemoryPressure();
  }

  bool OnCriticalMemoryPressure(size_t length) override {
    return platform_->OnCriticalMemoryPressure(length);
  }

  int NumberOfWorkerThreads() override {
    return platform_->NumberOfWorkerThreads();
  }

  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(v8::Isolate* isolate) override {
    return platform_->GetForegroundTaskRunner(isolate);
  }

  void CallOnWorkerThread(std::unique_ptr<v8::Task> task) override {
    return platform_->CallOnWorkerThread(std::move(task));
  }

  void CallBlockingTaskOnWorkerThread(std::unique_ptr<v8::Task> task) override {
    return platform_->CallBlockingTaskOnWorkerThread(std::move(task));
  }

  void CallLowPriorityTaskOnWorkerThread(std::unique_ptr<v8::Task> task) override {
    return platform_->CallLowPriorityTaskOnWorkerThread(std::move(task));
  }

  void CallDelayedOnWorkerThread(std::unique_ptr<v8::Task> task, double delay_in_seconds) override {
    return platform_->CallDelayedOnWorkerThread(std::move(task), delay_in_seconds);
  }

  bool IdleTasksEnabled(v8::Isolate* isolate) override {
    return platform_->IdleTasksEnabled(isolate);
  }

  std::unique_ptr<v8::JobHandle> PostJob(v8::TaskPriority priority, std::unique_ptr<v8::JobTask> job_task) override {
    return platform_->PostJob(priority, std::move(job_task));
  }

  double MonotonicallyIncreasingTime() override {
    return platform_->MonotonicallyIncreasingTime();
  }

  double CurrentClockTimeMillis() override {
    return platform_->CurrentClockTimeMillis();
  }

  StackTracePrinter GetStackTracePrinter() override {
    return platform_->GetStackTracePrinter();
  }

  v8::TracingController* GetTracingController() override {
    return platform_->GetTracingController();
  }

  void DumpWithoutCrashing() override {
    return platform_->DumpWithoutCrashing();
  }
};

int main(int argc, char* argv[]) {
  // Initialize V8.
  v8::V8::InitializeICUDefaultLocation(argv[0]);
  v8::V8::InitializeExternalStartupData(argv[0]);
  std::unique_ptr<v8::Platform> platform = std::make_unique<RestrictedPlatform>(v8::platform::NewDefaultPlatform());
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();

  parallel_experiment();

  // Dispose the isolate and tear down V8.
  v8::V8::Dispose();
  v8::V8::ShutdownPlatform();
  return 0;
}
