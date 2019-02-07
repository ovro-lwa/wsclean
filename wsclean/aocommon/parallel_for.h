#ifndef PARALLEL_FOR_H
#define PARALLEL_FOR_H

#include "barrier.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <sched.h>

#include <iostream>

namespace ao {

  template<typename Iter>
  class parallel_for
  {
  public:
    parallel_for(size_t nThreads) :
      _nThreads(nThreads), _barrier(nThreads, [&](){ _hasTasks=false; } ), _stop(false), _hasTasks(false)
    {
      startThreads();
    }
    
    ~parallel_for()
    {
      std::unique_lock<std::mutex> lock(_mutex);
      _stop = true;
      _hasTasks = true;
      _conditionChanged.notify_all();
      lock.unlock();
      for(std::thread& thr : _threads)
        thr.join();
    }

  /**
   * Iteratively call a function in parallel.
   * 
   * The function is expected to accept two size_t parameters, the loop
   * index and the thread id, e.g.:
   *   void loopFunction(size_t iteration, size_t threadID);
   * It is called (end-start) times.
   * 
   * This function is very similar to ThreadPool::For(), but does not
   * support recursion. For non-recursive loop, this function will be
   * faster.
   */
    void Run(Iter start, Iter end, std::function<void(size_t, size_t)> function)
    {
      std::unique_lock<std::mutex> lock(_mutex);
      _current = start;
      _end = end;
      _loopFunction = std::move(function);
      _hasTasks = true;
      _conditionChanged.notify_all();
      lock.unlock();
      loop(0);
      _barrier.wait();
    }
    
    size_t NThreads() const { return _nThreads; }
    
  private:
    parallel_for(const parallel_for&) = delete;
    
    void loop(size_t thread)
    {
      Iter iter;
      while(next(iter)) {
        _loopFunction(iter, thread);
      }
    }

    void run(size_t thread)
    {
      waitForTasks();
      while(!_stop) {
        loop(thread);
      _barrier.wait();
        waitForTasks();
      }
    }
    
    bool next(Iter& iter)
    {
      std::lock_guard<std::mutex> lock(_mutex);
      if(_current == _end)
        return false;
      else {
        iter = _current;
        ++_current;
        return true;
      }
    }
    
    void waitForTasks()
    {
      std::unique_lock<std::mutex> lock(_mutex);
      while(!_hasTasks)
        _conditionChanged.wait(lock);
    }
    
    void startThreads()
    {
      if(_nThreads > 1)
      {
        _threads.reserve(_nThreads-1);
        for(unsigned t=1; t!=_nThreads; ++t)
          _threads.emplace_back(&parallel_for::run, this, t);
      }
    }
    
    Iter _current, _end;
    std::mutex _mutex;
    size_t _nThreads;
    Barrier _barrier;
    std::atomic<bool> _stop;
    bool _hasTasks;
    std::condition_variable _conditionChanged;
    std::vector<std::thread> _threads;
    std::function<void(size_t, size_t)> _loopFunction;
  };
}

#endif
