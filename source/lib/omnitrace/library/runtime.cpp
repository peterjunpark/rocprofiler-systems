// MIT License
//
// Copyright (c) 2022 Advanced Micro Devices, Inc. All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "library/runtime.hpp"
#include "api.hpp"
#include "library/config.hpp"
#include "library/debug.hpp"
#include "library/defines.hpp"
#include "library/thread_data.hpp"
#include "library/utility.hpp"

#include <timemory/backends/dmp.hpp>
#include <timemory/backends/mpi.hpp>
#include <timemory/backends/process.hpp>
#include <timemory/backends/threading.hpp>
#include <timemory/components/rusage/backends.hpp>
#include <timemory/environment.hpp>
#include <timemory/sampling/allocator.hpp>
#include <timemory/settings.hpp>
#include <timemory/settings/types.hpp>
#include <timemory/utility/argparse.hpp>
#include <timemory/utility/declaration.hpp>
#include <timemory/utility/signals.hpp>

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <ostream>
#include <string>
#include <unistd.h>

namespace omnitrace
{
namespace
{
auto&
get_sampling_on_child_threads_history(int64_t _idx = utility::get_thread_index())
{
    static auto _v = utility::get_filled_array<OMNITRACE_MAX_THREADS>(
        []() { return utility::get_reserved_vector<bool>(32); });
    return _v.at(_idx);
}

bool&
sampling_on_child_threads()
{
    static thread_local bool _v = get_sampling_on_child_threads_history().empty()
                                      ? false
                                      : get_sampling_on_child_threads_history().back();
    return _v;
}
}  // namespace

int
get_realtime_signal()
{
    return SIGRTMIN + config::get_sampling_rtoffset();
}

int
get_cputime_signal()
{
    return SIGPROF;
}

std::set<int> get_sampling_signals(int64_t)
{
    auto _sigreal = get_realtime_signal();
    auto _sigprof = get_cputime_signal();

    if(config::get_use_sampling_realtime() && config::get_use_sampling_cputime())
        return std::set<int>{ _sigreal, _sigprof };
    else if(config::get_use_sampling_realtime())
        return std::set<int>{ _sigreal };
    else if(config::get_use_sampling_cputime())
        return std::set<int>{ _sigprof };

    return std::set<int>{};
}

std::atomic<uint64_t>&
get_cpu_cid()
{
    static std::atomic<uint64_t> _v{ 0 };
    return _v;
}

unique_ptr_t<std::vector<uint64_t>>&
get_cpu_cid_stack(int64_t _tid, int64_t _parent)
{
    struct omnitrace_cpu_cid_stack
    {};
    using init_data_t   = thread_data<bool, omnitrace_cpu_cid_stack>;
    using thread_data_t = thread_data<std::vector<uint64_t>, omnitrace_cpu_cid_stack>;

    static auto& _v = thread_data_t::instances(thread_data_t::construct_on_init{});
    static auto& _b = init_data_t::instances(init_data_t::construct_on_init{}, false);

    auto& _v_tid = _v.at(_tid);
    if(_b.at(_tid) && !(*_b.at(_tid)))
    {
        *_b.at(_tid)     = true;
        auto _parent_tid = _parent;
        // if tid != parent and there is not a valid pointer for the provided parent
        // thread id set it to zero since that will always be valid
        if(_tid != _parent_tid && !_v.at(_parent_tid)) _parent_tid = 0;
        // copy over the thread ids from the parent if tid != parent
        if(_tid != _parent_tid) *_v_tid = *_v.at(_parent_tid);
    }
    return _v_tid;
}

unique_ptr_t<cpu_cid_parent_map_t>&
get_cpu_cid_parents(int64_t _tid)
{
    struct omnitrace_cpu_cid_stack
    {};
    using thread_data_t = thread_data<cpu_cid_parent_map_t, omnitrace_cpu_cid_stack>;
    static auto& _v     = thread_data_t::instances(thread_data_t::construct_on_init{},
                                               cpu_cid_parent_map_t{});
    return _v.at(_tid);
}

std::tuple<uint64_t, uint64_t, uint32_t>
create_cpu_cid_entry(int64_t _tid)
{
    using tim::auto_lock_t;

    OMNITRACE_SCOPED_THREAD_STATE(ThreadState::Internal);

    // unique lock for _tid
    auto&       _mtx = get_cpu_cid_stack_lock(_tid);
    auto_lock_t _lk{ _mtx, std::defer_lock };
    if(!_lk.owns_lock()) _lk.lock();

    int64_t _p_idx = (get_cpu_cid_stack(_tid)->empty()) ? 0 : _tid;

    auto&       _p_mtx = get_cpu_cid_stack_lock(_p_idx);
    auto_lock_t _p_lk{ _p_mtx, std::defer_lock };
    if(!_p_lk.owns_lock()) _p_lk.lock();

    auto&&     _cid        = get_cpu_cid()++;
    auto&&     _parent_cid = get_cpu_cid_stack(_p_idx)->back();
    uint32_t&& _depth = get_cpu_cid_stack(_p_idx)->size() - ((_p_idx == _tid) ? 1 : 0);

    get_cpu_cid_parents(_tid)->emplace(_cid, std::make_tuple(_parent_cid, _depth));
    return std::make_tuple(_cid, _parent_cid, _depth);
}

cpu_cid_pair_t
get_cpu_cid_entry(uint64_t _cid, int64_t _tid)
{
    return get_cpu_cid_parents(_tid)->at(_cid);
}

tim::mutex_t&
get_cpu_cid_stack_lock(int64_t _tid)
{
    struct cpu_cid_stack_s
    {};
    return tim::type_mutex<cpu_cid_stack_s, project::omnitrace, max_supported_threads>(
        _tid);
}

namespace
{
void
setup_gotchas()
{
    static bool _initialized = false;
    if(_initialized) return;
    _initialized = true;

    OMNITRACE_BASIC_DEBUG(
        "Configuring gotcha wrapper around fork, MPI_Init, and MPI_Init_thread\n");

    component::mpi_gotcha::configure();
    component::exit_gotcha::configure();
    component::fork_gotcha::configure();
}
}  // namespace

std::unique_ptr<main_bundle_t>&
get_main_bundle()
{
    static auto _v = []() {
        auto _self = RUSAGE_SELF;
        std::swap(_self, tim::get_rusage_type());
        auto _tmp = std::make_unique<main_bundle_t>(
            JOIN('/', "omnitrace/process", process::get_id()),
            quirk::config<quirk::auto_start>{});
        std::swap(_self, tim::get_rusage_type());
        return _tmp;
    }();
    return _v;
}

std::unique_ptr<init_bundle_t>&
get_init_bundle()
{
    static auto _v = std::make_unique<init_bundle_t>(
        JOIN('/', "omnitrace/process", process::get_id()));
    return _v;
}

std::unique_ptr<preinit_bundle_t>&
get_preinit_bundle()
{
    static auto _v =
        (setup_gotchas(), std::make_unique<preinit_bundle_t>(
                              JOIN('/', "omnitrace/process", process::get_id()),
                              quirk::config<quirk::auto_start>{}));
    return _v;
}

namespace
{
auto&
get_thread_state_history(int64_t _idx = utility::get_thread_index())
{
    static auto _v = utility::get_filled_array<OMNITRACE_MAX_THREADS>(
        []() { return utility::get_reserved_vector<ThreadState>(32); });

    return _v.at(_idx);
}
}  // namespace

ThreadState&
get_thread_state()
{
    static thread_local ThreadState _v{ ThreadState::Enabled };
    return _v;
}

ThreadState
set_thread_state(ThreadState _n)
{
    auto _o            = get_thread_state();
    get_thread_state() = _n;
    return _o;
}

ThreadState
push_thread_state(ThreadState _v)
{
    if(get_thread_state() >= ThreadState::Completed) return get_thread_state();

    return get_thread_state_history().emplace_back(set_thread_state(_v));
}

ThreadState
pop_thread_state()
{
    if(get_thread_state() >= ThreadState::Completed) return get_thread_state();

    auto& _hist = get_thread_state_history();
    if(!_hist.empty())
    {
        set_thread_state(_hist.back());
        _hist.pop_back();
    }
    return get_thread_state();
}

bool
sampling_enabled_on_child_threads()
{
    return sampling_on_child_threads();
}

bool
push_enable_sampling_on_child_threads(bool _v)
{
    bool _last                  = sampling_on_child_threads();
    sampling_on_child_threads() = _v;
    auto& _hist                 = get_sampling_on_child_threads_history();
    _hist.emplace_back(_last);
    return _last;
}

bool
pop_enable_sampling_on_child_threads()
{
    auto& _hist = get_sampling_on_child_threads_history();
    if(!_hist.empty())
    {
        bool _restored = _hist.back();
        _hist.pop_back();
        sampling_on_child_threads() = _restored;
    }
    return sampling_on_child_threads();
}

void
set_sampling_on_all_future_threads(bool _v)
{
    for(size_t i = 0; i < max_supported_threads; ++i)
        get_sampling_on_child_threads_history(i).emplace_back(_v);
}
}  // namespace omnitrace
