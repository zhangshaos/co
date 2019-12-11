#include "scheduler.h"

DEF_uint32(co_sched_num, os::cpunum(), "number of coroutine schedulers, default: cpu num");
DEF_uint32(co_stack_size, 1024 * 1024, "size of the stack shared by coroutines, default: 1M");

namespace co {

timer_id_t null_timer_id;
__thread Scheduler* gSched = 0;

Scheduler::Scheduler(uint32 id)
    : _id(id), _stack(0), _running(0), _wait_ms(-1),
      _stop(false), _timeout(false) {
    _main_co = new Coroutine();
    _co_pool.reserve(1024);
    _co_pool.push_back(_main_co);
    _co_ids.reserve(1024);
    _it = _timed_wait.end();
}

Scheduler::~Scheduler() {
    this->stop();
    free(_stack);
    for (size_t i = 0; i < _co_pool.size(); ++i) {
        delete _co_pool[i];
    }
}

void Scheduler::stop() {
    if (atomic_swap(&_stop, true) == false) {
        _epoll.signal();
        _ev.wait();
    }
}

static void main_func(tb_context_from_t from) {
    // from.ctx  == this callee's context?
    // from.priv == _main_co（初始化为空协程）
    ((Coroutine*)from.priv)->ctx = from.ctx; // 从这儿开始之后，main_co.ctx = callee.ctx，也就是说main_co正式变成callee协程了
    gSched->running()->cb->run();       // 执行协程函数
    gSched->recycle(gSched->running()); // recycle the current coroutine，协程函数正常执行完后，回收这个协程（如果协程阻塞，会直接调用yield()返回到main_co)
    tb_context_jump(from.ctx, 0);       // jump back to the main context(callee.ctx)
}

inline void Scheduler::save_stack(Coroutine* co) {
    if (co->stack) {
        co->stack->clear();
    } else {
        co->stack = new fastream(_stack + FLG_co_stack_size - (char*)co->ctx);
    }
    co->stack->append(co->ctx, _stack + FLG_co_stack_size - (char*)co->ctx); // 保存协程的栈
}

/*
 *  scheduler thread:
 *    resume(co) -> jump(co->ctx, main_co)
 *       ^             |
 *       |             v
 *  jump(main_co)  main_func(from): from.priv == main_co
 *    yield()          |
 *       |             v
 *       <-------- co->cb->run():  run on _stack
 */
void Scheduler::resume(Coroutine* co) {
    tb_context_from_t from;
    _running = co;
    if (_stack == 0) _stack = (char*) malloc(FLG_co_stack_size);

    // @ctx 是指向栈顶的指针
    if (co->ctx == 0) {
        // 如果协程是第一次被调度
        co->ctx = tb_context_make(_stack, FLG_co_stack_size, main_func); // main_func将被放在_stack中执行
        from = tb_context_jump(co->ctx, _main_co); // 转去执行main_func的上下文,并且将目前的上下文（相对main_func的callee.ctx）保存在from中
    } else {
        // restore stack for the coroutine
        assert((_stack + FLG_co_stack_size) == (char*)co->ctx + co->stack->size());
        memcpy(co->ctx, co->stack->data(), co->stack->size());
        from = tb_context_jump(co->ctx, _main_co);
    }

    if (from.priv) {
        // 如果running的协程没有执行完（因为阻塞...等原因调用yield()退出到main_co(即此处)）
        // void yield() {
        //  tb_context_jump(_main_co->ctx, _running);
        // }
        assert(_running == from.priv);
        _running->ctx = from.ctx;   // update context for the coroutine // 因为yield中并没有更新上下文
        this->save_stack(_running); // save stack of the coroutine        
    }
}

void Scheduler::loop() {
    gSched = this;
    std::vector<Closure*> task_cb;
    std::vector<Coroutine*> task_co;
    std::unordered_map<Coroutine*, timer_id_t> co_ready;

    while (!_stop) {
        int n = _epoll.wait(_wait_ms);
        if (_stop) break;

        if (unlikely(n == -1)) {
            ELOG << "iocp wait error: " << co::strerror();
            continue;
        }

        for (int i = 0; i < n; ++i) { 
            // 首先调度所有有I/O事件(忽视掉管道信号)的协程,为什么要忽视掉管道事件？
            // 因为管道事件pipe[2]注册到epoll中的目的是及时唤醒loop，当loop被唤醒后，管道中距离的数据也就不重要了
            auto& ev = _epoll[i];
            if (_epoll.has_ev_pipe(ev)) {
                _epoll.handle_ev_pipe();
                continue;
            }

          #if defined(_WIN32)
            PerIoInfo* info = (PerIoInfo*) _epoll.ud(ev);
            info->n = ev.dwNumberOfBytesTransferred;
            this->resume(info->co);

          #elif defined(__linux__)
            uint64 ud = _epoll.ud(ev);
            // 唤醒_co_pool中的读写阻塞进程
            if (_epoll.has_ev_read(ev)) this->resume(_co_pool[(uint32)(ud >> 32)]);
            if (_epoll.has_ev_write(ev)) this->resume(_co_pool[(uint32)ud]);

          #else
            this->resume((Coroutine*)_epoll.ud(ev));
          #endif
        }

        do {
            // 接着调度所有新增加/等待恢复的协程
            {
                ::MutexGuard g(_task_mtx);
                if (!_task_cb.empty()) _task_cb.swap(task_cb);
                if (!_task_co.empty()) _task_co.swap(task_co);
            }

            if (!task_cb.empty()) {
                for (size_t i = 0; i < task_cb.size(); ++i) {
                    this->resume(this->new_coroutine(task_cb[i]));
                }
                task_cb.clear();
            }

            if (!task_co.empty()) {
                for (size_t i = 0; i < task_co.size(); ++i) {
                    this->resume(task_co[i]);
                }
                task_co.clear();
            }
        } while (0);

        do {
            // 唤醒_co中所有等待唤醒的协程
            {
                ::MutexGuard g(_co_mtx);
                if (!_co.empty()) _co.swap(co_ready);
            }
            if (!co_ready.empty()) {
                for (auto it = co_ready.begin(); it != co_ready.end(); ++it) {
                    if (it->first->ev != ev_ready) continue;
                    // time_id_t实际上是一个迭代器
                    if (it->second != null_timer_id) _timed_wait.erase(it->second);
                    // 从等待唤醒的协程组中删除掉
                    this->resume(it->first);
                }
                co_ready.clear();
            }
        } while (0);

        do {
            // 调度所有到时的协程
            this->check_timeout(task_co);

            if (!task_co.empty()) {
                _timeout = true;
                for (size_t i = 0; i < task_co.size(); ++i) {
                    this->resume(task_co[i]);
                }
                _timeout = false;
                task_co.clear();
            }
        } while (0);
    }

    _ev.signal();
}


void Scheduler::check_timeout(std::vector<Coroutine*>& res) {
    if (_timed_wait.empty()) {
        if (_wait_ms != (uint32)-1) _wait_ms = -1;
        return;
    }

    do {
        int64 now_ms = now::ms();

        // 将_timed_wait中到时的协程全部放入@res中
        auto it = _timed_wait.begin();
        for (; it != _timed_wait.end(); ++it) {
            if (it->first > now_ms) break;
            Coroutine* co = it->second;
            if (co->ev != 0) atomic_swap(&co->ev, 0); //enum _Event_status: ev_wait = 1; ev_ready = 2 // ev = 0应该表示这不是一个协程
            res.push_back(co);
        }

        if (it != _timed_wait.begin()) {
            if (_it != _timed_wait.end() && _it->first > now_ms) {
                _it = it;  // 让_it重新指向第一个大于now的协程
            }
            // 从_tiemd_wait中删除那些时间小于now的协程
            _timed_wait.erase(_timed_wait.begin(), it);
        }

        if (!_timed_wait.empty()) {
            _wait_ms = (int) (_timed_wait.begin()->first - now_ms);
        } else {
            if (_wait_ms != (uint32)-1) _wait_ms = -1;
        }

    } while (0);
}

SchedulerMgr::SchedulerMgr() : _index(-1) {
    if (FLG_co_sched_num > (uint32) os::cpunum()) FLG_co_sched_num = os::cpunum();
    if (FLG_co_stack_size == 0) FLG_co_stack_size = 1024 * 1024;

    _n = FLG_co_sched_num;
    (_n && (_n - 1)) ? (_n = -1) : --_n; // 如果_n = 1 -> _n = 0;如果_n > 1 -> _n = (uint)-1

  #ifdef _WIN32
    _Wsa_startup();
  #endif

    LOG << "coroutine schedulers start, sched num: " << FLG_co_sched_num
        << ", stack size: " << (FLG_co_stack_size >> 10) << 'k';

    for (uint32 i = 0; i < FLG_co_sched_num; ++i) {
        Scheduler* s = new Scheduler(i);
        s->loop_in_thread();
        _scheds.push_back(s);
    }
}

SchedulerMgr::~SchedulerMgr() {
    for (size_t i = 0; i < _scheds.size(); ++i) {
        delete _scheds[i];
    }

  #ifdef _WIN32
    _Wsa_cleanup();
  #endif
}

inline SchedulerMgr& sched_mgr() {
    static SchedulerMgr kSchedMgr;
    return kSchedMgr;
}

void go(Closure* cb) {
    sched_mgr()->add_task(cb);
}

void sleep(uint32 ms) {
    gSched != 0 ? gSched->sleep(ms) : sleep::ms(ms);
}

void stop() {
    auto& scheds = co::schedulers();
    for (size_t i = 0; i < scheds.size(); ++i) {
        scheds[i]->stop();
    }
}

std::vector<Scheduler*>& schedulers() {
    return sched_mgr().schedulers();
}

int sched_id() {
    return gSched->id();
}

} // co
