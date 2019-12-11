> **co库代码分析**
>
> 作者：章星明
>
> 工作：co库中协程的完整分析和代码注释
>
> 备注：https://github.com/zhangshaos/co

[toc]



# Co协程基本原理：

**Scheduler::loop_in_thread直接开一个调度线程去执行其成员函数loop()，loop()中调度各个协程：**

* ScheduleMgr构造函数中调用CPU数量的Schedule::loop_in_thread

* ScheduleMgr管理所有协程调度线程，Schedule表示具体的一个调度线程

> Q：如何把函数送到调度线程中的?
>
> A：使用go(func_xxx)

## go原理：

go-->调用new_callback()-->将参数转为一个Closure对象-->

再调用go(Closure)-->在sched_mgr()（全局的ScheduleMgr对象）拥有Scheduler中轮流选一个，并在其中增加一个task（add_task(Closure)）

```c++
inline void go(void (*f)()) {
    go(new_callback(f));
}
void go(Closure* cb) {
    sched_mgr()->add_task(cb);
}
```



## Scheduler类:

#### go调用Schedule

scheduler在收到go的add_task(Closure)请求后，将新增加的Closure放入其`_task_cb`中，并触发`_epoll`的signal()信号

> 注释：`X`表示给定类的成员变量

```c++
inline void Scheduler::add_task(Closure* cb) {
    {
        ::MutexGuard g(_task_mtx);
       _task_cb.push_back(cb);
    }
    _epoll.signal();
}
```

> _epoll.signal()的解释：

> Epoll::signal()--向Scheduler::@\_pipe[2]中写入一字节-->Scheduler::loop()中的@_epoll.wait()苏醒,然后loop()就可以调用resume来调度各个协程

```c++
void Scheduler::loop() {
	/*...省略...*/
    while (!_stop) {
        int n = _epoll.wait(_wait_ms);
        /*...省略...*/
    }
}
```



#### Schedule::loop()被唤醒

1. 首先resume `_co_pool`中的协程

```c++
// 因为管道事件pipe[2]注册到epoll中的目的是及时唤醒loop，当loop被唤醒后，管道中具体的数据也就不重要了
auto& ev = _epoll[i];
if (_epoll.has_ev_pipe(ev)) {
	_epoll.handle_ev_pipe();
	continue;
}

/*...省略...*/

#elif defined(__linux__)
uint64 ud = _epoll.ud(ev);
// 唤醒_co_pool中的读写阻塞进程
if (_epoll.has_ev_read(ev)) {
    this->resume(_co_pool[(uint32)(ud >> 32)]);
}
if (_epoll.has_ev_write(ev)) {
    this->resume(_co_pool[(uint32)ud]);
}
```

**Q: _co_pool有什么用？**

> 1.用来存储Coroutine对象的仓库，当有新的Closure对象需要变成Coroutine时（例如new_coroutine调用，两者区别参考下方第二个问题），就从这里面取出一个对象用Closure初始化一下并返回

> 2.保留那些还未执行结束（I/O阻塞）的协程（细节请参见下面内容中关于new_coroutine、resume、yield函数的解析）

> PS：_co_ids是辅助\_co_pool的一个数组，表示仓库中那些被回收（协程正常结束后被recycle函数回收）的Coroutine对象（下一次可以直接用来完成new_coroutine的转化任务）

**Q: _co_pool中的协程从何而来？**

> 1.一开始包含一个_main_co（空协程，在Schedule构造函数中初始化，在main_func中被修改）

> 2.还可以通过new_croutine转loop化得来（如果\_co_ids回收的协程集为空，用Closure new一个新的Coroutine，并将其加入\_co_pool中）(注意，通过转化来的协程在阻塞后，依然是留在_co_pool中)

```c++
inline Coroutine* Scheduler::new_coroutine(Closure* cb) {
    if (!_co_ids.empty()) {
        // ～～如果_co_pool不是空的～～
        // 注意：其实此处的意思是仅仅是_co_ids为空，_co_ids和_co_pool不同步，但是可以近似这样理解，下同
        Coroutine* co = _co_pool[_co_ids.back()];
        co->cb = cb;
        co->ctx = 0;
        co->ev = 0;
        _co_ids.pop_back();
        return co;
    } else {
        // ～～如果_co_pool是空的～～（第一次函数调用来到这里）
        Coroutine* co = new Coroutine((int)_co_pool.size(), cb); // 第一次调用这个，_co_pool.size == 1(因为有_main_co)
        // 新Coroutine的id会在recycle中被蓄积到_co_ids中
        _co_pool.push_back(co); // 注意此处
        return co;
    }
}
```

---


2. 然后将task_cb中的协程转化后（利用Scheduler::new_coroutine，将Closure变成Coroutine（注意：完成转化后的Coroutine同时转移到_co_pool中了）），再resume它们

**Q: Closure和Coroutine有什么区别？**

> Closure只是一个包装函数的对象，而Coroutine是真正的协程，包含一个协程栈

```c++
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
```

---

3. resume task_co中的协程
```c++
            if (!task_co.empty()) {
                for (size_t i = 0; i < task_co.size(); ++i) {
                    this->resume(task_co[i]);
                }
                task_co.clear();
            }
```
**Q : task_co中的协程从何而来？**

> Scheduler::add_task(Coroutine* co) <--从MutexImpl::_co_wait（阻塞在mutex上的协程）中取得协程--- MutexImpl::unlock()，也就是说task_co表示由于**互斥量阻塞**的协程集

> Q：_task_cb和\_task_co有什么区别？
>
> A : 前者指的是使用go第一次加入调度的协程，而后者指的是因为阻塞在**互斥量**上的协程。

---

4. 遍历`_co`中准备好(ev_ready)再次调度的协程，将他们从`_timed_wait`中删除，然后resume 这些协程

```c++
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
```

**Q: _co中的协程从哪里来？**

> Schedule::add_task(Coroutine* co, timer_id_t id) <---将EventImpl::_co_wait中状态为ev_wait的协程状态变为ev_ready，并加入Schedule::\_co中---EventImpl::signal()<--唤醒所有在等待(wait)的协程--Event::signal()，也就是说\_co表示由于**协程同步事件**而阻塞的协程。

```c++
void EventImpl::signal() {
    std::unordered_map<Coroutine*, timer_id_t> co_wait;
    {
        ::MutexGuard g(_mtx);
        _co_wait.swap(co_wait); // _co_wait表示在this Event上等待的协程
    }

    for (auto it = co_wait.begin(); it != co_wait.end(); ++it) {
        Coroutine* co = it->first;
        if (atomic_compare_swap(&co->ev, ev_wait, ev_ready) == ev_wait) { // 如果协程之前的状态是wait，则变成ready，并且进入这里。
            co->s->add_task(co, it->second); // 在Schedule._co中增加一项co
        }
    }
}
```

---

5. 调度`_timed_wait`中的所有到时的(timeout)协程

```c++
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
```

```c++
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
            if (co->ev != 0) atomic_swap(&co->ev, 0); //enum _Event_status: ev_wait = 1; ev_ready = 2 // ev = 0应该表示这不是一个正常的协程，例如空协程
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
```

> Q：_timed_wait中的协程从何而来？

> _timed_wait.insert(std::make_pair(now::ms() + ms, _running))<---Schedule::sleep(ms)，Schedule::add_timer(ms)，Schedule::add_ev_timer(ms)<--调用add_timer或者add_ev_timer---EvWrite/EvRead::wait(ms)，EventImple::wait(ms)（只调用add_ev_timer）
>
> 也就是说_timed_wait中的协程大部分都来自于由于**定时等待**导致阻塞的协程们

---

#### resume()函数解析

```c++
/*
 *  scheduler thread:
 *    resume(co)  -->  jump(co->ctx, main_co)
 *       ^            		 	|
 *       |            		  	  v
 *  jump(main_co)  main_func(from): from.priv == main_co
 *    yield()        		    |	
 *       |            			 v
 *       <-------- 	co->cb->run():  run on _stack
 */
void Scheduler::resume(Coroutine* co) {
    tb_context_from_t from;
    _running = co;
    if (_stack == 0) _stack = (char*) malloc(FLG_co_stack_size);

    // @ctx 是指向栈顶的指针
    if (co->ctx == 0) {
        // 如果协程是第一次被调度
        co->ctx = tb_context_make(_stack, FLG_co_stack_size, main_func); // main_func将被放在_stack中执行
        from = tb_context_jump(co->ctx, _main_co); // 转去执行main_func的上下文
    } else {
        // restore stack for the coroutine
        assert((_stack + FLG_co_stack_size) == (char*)co->ctx + co->stack->size());
        memcpy(co->ctx, co->stack->data(), co->stack->size());
        from = tb_context_jump(co->ctx, _main_co);
    }

    if (from.priv) {
        // 如果running的协程没有执行完（因为阻塞...等原因调用yield()退出到main_co(即此处)，正常执行时from.priv == 0，参考）
        assert(_running == from.priv);
        _running->ctx = from.ctx;   // update context for the coroutine // 因为yield中并没有更新上下文
        this->save_stack(_running); // save stack of the coroutine        
    }
}

static void main_func(tb_context_from_t from) {
    // from.ctx  ==  callee's context
    // from.priv == _main_co（初始化为空协程）
    ((Coroutine*)from.priv)->ctx = from.ctx; // 从这儿开始之后，main_co.ctx == callee.ctx，也就是说_main_co正式变成callee协程了
    gSched->running()->cb->run();       // 执行协程函数
    gSched->recycle(gSched->running()); // recycle the current coroutine，协程函数正常执行完后，回收这个协程（如果协程阻塞，会直接调用yield()返回到_main_co)
    tb_context_jump(from.ctx, 0);       // jump back to the main context(callee.ctx)
}

    void yield() {
        tb_context_jump(_main_co->ctx, _running);  // _running = co; 在resume(co)函数中
    }
```

*注意：Schedule构造函数中，_main_co被初始化为一个空协程*

**重要注释：**

> `from = tb_context_jump(co->ctx, _main_co)`
>
> @_main_co是调用这个jump的协程（准确来说，它是在main_func中被更新为调用者协程）这大概是在模拟call func时，将ip入栈，这样co->ctx也可以回到调用者
>
> @from.priv表示因阻塞而未执行完的协程，即co/_running（如果协程正常执行完，则此项为0）
>
> @from.ctx表示该阻塞协程的最新上下文

---

resume过程如下：

1. 如果resume co（co是第一次被调度），那么使用main_func创建co运行上下文（即co在main_func中运行）
2. tb_context_jump内部将当前（调用者）上下文和_main_co（调用者协程）传入main_func参数中，并开始执行main_func
3. main_func使用调用者上下文更新_main_co.ctx
4. 协程co开始运行
5. 如果co正常结束，则回收这个协程到_co_ids中，然后返回到调用者上下文，最后resume函数结束
6. 如果co阻塞，其内部会调用yield()，直接返回到调用者上下文，然后用阻塞时的状态更新co的上下文和栈（下次被调度时就直接回到该状态）

---

#### 其他问题

**Q : Coroutine如何创建的？**

> 第一次调用new_coroutine时，由于_co_ids(回收的协程集) == 0（其实此时\_co_pool不为空，因为有\_main_co），所以会用Closure new一个新的Coroutine，并用\_co_pool.size作为新协程的id，之后将其加入\_co_poll中

**Q : 当一个协程执行完了，如何退出？**
>
> A：当协程执行完毕后，main_func中`gSched->recycle(gSched->running())`，recycle中回收的协程加入到_co_ids中
>
> PS: 为什么recycle回收的协程id刚好和_co_pool对应？参考创建Coroutine的过程

---

**Q：协程采用时间片轮转吗？这段时间内没有执行完的协程放在哪里？**
>此协程库不使用时间片轮转算法。当协程因为I/O或互斥量而阻塞时，会自己调用gSched->yield()退出来

---

**Q：co库是如何知道某个协程阻塞了?**
> 由于整个系统都采用异步的读和写，通过异步读写返回值判断读写无法进行（阻塞），然后I/O函数会调用EvWrite/EvRead类的wait()函数。函数中：`__thread gSched->add_ev_write/read(fd)`，通过此函数调用，将读写事件注册到_epoll监测中。之后调用Schedule::yield()函数将协程退出。

```c++
void EvWrite::wait() {
        if (!_has_ev) {
            _has_ev = true;
            gSched->add_ev_write(_fd);
        }
        gSched->yield();
    }
```

> 注意：上一个问题的gSched是一个线程局部变量
>
> TLS原理：编译时将TLS变量放置与tls段中，当新线程创建时，将tls复制一份到堆中。当访问某个TLS变量时，从线程环境块TEB中取得该线程tls-copy的地址，加上该变量在原本tls段中的偏移，既可以访问tls-copy中的这个变量

---

**Q: 当_epoll.wait()被唤醒, co库是如何知道对应的是哪个Coroutine?**

> 接上文`co库如何知道线程阻塞了？`，Schedule::add_ev_write/read()中，调用了`Epoll::add_ev_write(fd, _running->id)`，将正在运行的Coroutine的id写入epoll_event.data中

> 当_epoll监测到事件时，返回的epoll_event.data中就会带有上一步注册的Coroutine的id。

**Q: epoll.data.u64有64位，一个id32位，剩下的32位用来干什么？**

> Epoll::_ev_map: <fd, epoll_event.data.u64> 其中u64的高32位表示“此fd上注册读事件的Coroutine ID”，低32位表示“此fd上注册写事件的Coroutine ID”

---

**对比两种signal函数**

> Event::signal() : 唤醒所有等待的协程，用于协程的事件同步

> Epoll::signal() : 唤醒Schedule::loop中的_epoll.wait()，用户唤醒协程调度线程
>