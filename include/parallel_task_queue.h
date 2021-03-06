#ifndef AMLIB_PARALLEL_TASK_QUEUE_H_
#define AMLIB_PARALLEL_TASK_QUEUE_H_

#include <deque>
#include <vector>
#include <algorithm>
#include <iostream>
#include <functional>

#include "task_thread.h"


namespace am {


/*****************************************************************************
 *
 * @brief    simple queue that holds tasks and executes them in parallel
 *
 * @tparam   Task : assumed to be cheap to copy or move
 *                  (e.g. std::function, std::reference_wrapper, ...)
 *                  Task::operator()() must be defined
 *
 *****************************************************************************/
template<class Task>
class parallel_task_queue
{
public:
    //---------------------------------------------------------------
    using task_type = Task;


private:
    //---------------------------------------------------------------
    class task_executor {
    public:
        explicit
        task_executor(parallel_task_queue* callback = nullptr,
                      task_type&& task = task_type{}) noexcept
        :
            queue_{callback},
            task_{std::move(task)}
        {}

        void operator () () {
            task_();
            --queue_->running_;
        }

    private:
        parallel_task_queue* queue_;
        task_type task_;
    };

    friend class task_executor;


public:
    //---------------------------------------------------------------
    explicit
    parallel_task_queue(unsigned int concurrency =
        std::thread::hardware_concurrency())
    :
        waitMtx_{}, enqueueMtx_{},
        active_{true}, hasWaiting_{false},
        running_{0},
        waiting_{},
        workers_(concurrency),
        isDone_{},
        scheduler_{ [&] { schedule(); }}
    {}

    //-----------------------------------------------------
    parallel_task_queue(const parallel_task_queue&) = delete;
    parallel_task_queue(parallel_task_queue&&) = delete;


    //-----------------------------------------------------
    ~parallel_task_queue()
    {
        clear();
        //make sure that scheduler wakes up and terminates
        active_.store(false);
        //wait for scheduler to terminate
        scheduler_.join();
    }


    //---------------------------------------------------------------
    parallel_task_queue& operator = (const parallel_task_queue&) = delete;
    parallel_task_queue& operator = (parallel_task_queue&&) = delete;


    //---------------------------------------------------------------
    void
    enqueue(const task_type& t)
    {
        std::lock_guard<std::recursive_mutex> lock(enqueueMtx_);
        waiting_.push_back(t);
        hasWaiting_.store(true);
    }
    //-----------------------------------------------------
    void
    enqueue(task_type&& t)
    {
        std::lock_guard<std::recursive_mutex> lock(enqueueMtx_);
        waiting_.push_back(std::move(t));
        hasWaiting_.store(true);
    }
    //-----------------------------------------------------
    template<class InputIterator>
    void
    enqueue(InputIterator first, InputIterator last)
    {
        std::lock_guard<std::recursive_mutex> lock(enqueueMtx_);
        waiting_.insert(begin(waiting_), first, last);
        hasWaiting_.store(true);
    }
    //-----------------------------------------------------
    void
    enqueue(std::initializer_list<task_type> il)
    {
        std::lock_guard<std::recursive_mutex> lock(enqueueMtx_);
        waiting_.insert(waiting_.end(), il);
        hasWaiting_.store(true);
    }
    //-----------------------------------------------------
    template<class... Args>
    void
    enqueue_emplace(Args&&... args)
    {
        std::lock_guard<std::recursive_mutex> lock(enqueueMtx_);
        waiting_.emplace_back(std::forward<Args>(args)...);
        hasWaiting_.store(true);
    }


    /****************************************************************
     * @brief removes first waiting task that compares equal to 't'
     */
    bool
    try_remove(const task_type& t)
    {
        std::lock_guard<std::recursive_mutex> lock(enqueueMtx_);
        auto it = std::find(waiting_.begin(), waiting_.end(), t);
        if(it != waiting_.end()) {
            waiting_.erase(it);
            if(waiting_.empty()) hasWaiting_.store(false);
            return true;
        }
        return false;
    }


    //---------------------------------------------------------------
    void
    clear() {
        std::lock_guard<std::recursive_mutex> lock(enqueueMtx_);
        waiting_.clear();
        hasWaiting_.store(false);
    }


    //---------------------------------------------------------------
    unsigned int
    concurrency() const noexcept {
        return workers_.size();
    }

    //---------------------------------------------------------------
    bool
    empty() const noexcept {
        return !hasWaiting_.load();
    }


    //-----------------------------------------------------
    /// @return number of waiting tasks
    std::size_t
    waiting() const noexcept {
        std::lock_guard<std::recursive_mutex> lock{enqueueMtx_};
        return waiting_.size();
    }
    std::size_t
    unsafe_waiting() const noexcept {
        return waiting_.size();
    }

    //-----------------------------------------------------
    /// @return number of running tasks
    std::size_t
    running() const noexcept {
        return running_.load();
    }

    //-----------------------------------------------------
    /// @return true, if all threads are working on a task
    bool
    busy() const noexcept {
        return running_.load() >= int(workers_.size());
    }

    //-----------------------------------------------------
    bool
    complete() const noexcept {
        std::lock_guard<std::recursive_mutex> lock{enqueueMtx_};
        return empty() && (running() < 1);
    }


    //---------------------------------------------------------------
    /**
     * @brief block execution of calling thread until all tasks are complete
     */
    void wait()
    {
        std::unique_lock<std::mutex> lock{waitMtx_};
        while(!empty() || running()) isDone_.wait(lock);
    }


private:

    //---------------------------------------------------------------
    void try_assign_tasks()
    {
        for(auto& worker : workers_) {
            if(worker.available()) {
                std::lock_guard<std::recursive_mutex> lock{enqueueMtx_};
                if(waiting_.empty()) {
                    hasWaiting_.store(false);
                    return;
                }
                if(worker(this, std::move(waiting_.front()))) {
                    ++running_;
                    waiting_.pop_front();
                    if(waiting_.empty()) {
                        hasWaiting_.store(false);
                        return;
                    }
                    else if(busy()) {
                        return;
                    }
                }
            }
        }
    }

    //---------------------------------------------------------------
    /// @brief this will run in a separate, dedicated thread
    void schedule()
    {
         while(active_.load()) {
             if(!empty() && !busy()) {
                 try_assign_tasks();
             }
             else if(running() < 1) {
                 std::lock_guard<std::recursive_mutex> lock{enqueueMtx_};
                 if(empty() && (running() < 1)) {
                     isDone_.notify_all();
                 }
             }
         }
    }


    //---------------------------------------------------------------
    mutable std::mutex waitMtx_;
    mutable std::recursive_mutex enqueueMtx_;
    std::atomic_bool active_;
    std::atomic_bool hasWaiting_;
    std::atomic_int running_;
    std::deque<task_type> waiting_;
    std::vector<task_thread<task_executor>> workers_;
    std::condition_variable isDone_;
    std::thread scheduler_;
};



//-------------------------------------------------------------------
/// @brief convenience alias
using parallel_function_queue = parallel_task_queue<std::function<void()>>;


} // namespace am


#endif
