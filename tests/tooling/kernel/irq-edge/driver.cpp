// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
extern "C" {
int os_setup_epoll(); int os_add_epoll_fd(int,int,void*);
int os_del_epoll_fd(int); int os_waiting_for_events_epoll(); void os_close_epoll_fd();
void *os_epoll_get_data_pointer(int); int os_epoll_triggered(int,int);
int darwin_irq_set_notification(int,int); int os_irq_wake_pending();
void darwin_irq_idle_begin(sigset_t*); void darwin_irq_idle_end(const sigset_t*);
int darwin_irq_dispatch(int,unsigned long long,void*);
void panic(const char*,...) { std::abort(); }
void os_set_thread_name(const char*) {}
long long os_nsecs(void) {
 timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000000000LL+t.tv_nsec;
}
}
// What the dispatch calls, and the idle secondary the notification thread wakes.
static std::atomic<int> dispatches{0}, wakes{0}, nested{-1};
extern "C" void sigio_handler(int,void*,void*,void*) {
 dispatches++;
 // A CPU that dispatches while another does must not, whatever its time.
 if (nested.load()>=0) nested = darwin_irq_dispatch(0,1ULL<<62,nullptr);
}
extern "C" int darwin_wake_idle_secondary(void) { return ++wakes==0 ? -1 : 1; }
static void sleep_ms(int ms) { timespec t{0,ms*1000000L}; nanosleep(&t,nullptr); }
static bool wait_for_wake(int count) {
 for (int i=0;i<5000 && wakes<count;i++) sleep_ms(1);
 return wakes>=count;
}
int main() {
 int fds[2]; assert(pipe(fds)==0); int token=1, next=2;
 assert(os_add_epoll_fd(1,fds[0],&token)==0);
 assert(write(fds[1],"a",1)==1);
 assert(os_setup_epoll()==0);
 // A descriptor that notifies and has input is a wake source. A system
 // suspend clears the notification of the others, and setting it restores it.
 assert(os_irq_wake_pending()==1);
 assert(darwin_irq_set_notification(fds[0],0)==0 && os_irq_wake_pending()==0);
 assert(darwin_irq_set_notification(fds[0],1)==0 && os_irq_wake_pending()==1);
 assert(os_waiting_for_events_epoll()==1);
 assert(os_epoll_get_data_pointer(0)==&token && os_epoll_triggered(0,1));
 // Reentry before the outer handler drains must not deliver the same level.
 assert(os_waiting_for_events_epoll()==0);
 // Adding a registered descriptor modifies it and rearms its edge.
 assert(os_add_epoll_fd(1,fds[0],&next)==0);
 assert(os_waiting_for_events_epoll()==1);
 assert(os_epoll_get_data_pointer(0)==&next);
 assert(os_waiting_for_events_epoll()==0);
 char byte; assert(read(fds[0],&byte,1)==1);
 // Drain and refill between polls must still produce a fresh edge.
 assert(write(fds[1],"b",1)==1);
 assert(os_waiting_for_events_epoll()==1);
 assert(os_waiting_for_events_epoll()==0);
 assert(read(fds[0],&byte,1)==1);
 close(fds[1]); assert(os_waiting_for_events_epoll()==1);
 assert(os_waiting_for_events_epoll()==0);
 assert(os_del_epoll_fd(fds[0])==0); close(fds[0]);
 // The dispatch token: CPU0 always dispatches; another CPU only once no
 // dispatch has begun for 20 ms; none while one runs; the stamp never goes back.
 constexpr unsigned long long ms=1000000ULL, t0=1000*ms;
 assert(darwin_irq_dispatch(0,t0,nullptr)==1 && dispatches==1);
 assert(darwin_irq_dispatch(1,t0+19*ms,nullptr)==0 && dispatches==1);
 assert(darwin_irq_dispatch(1,t0+20*ms,nullptr)==1 && dispatches==2);
 assert(darwin_irq_dispatch(1,t0+39*ms,nullptr)==0 && dispatches==2);
 assert(darwin_irq_dispatch(0,t0+5*ms,nullptr)==1 && dispatches==3);
 assert(darwin_irq_dispatch(1,t0+39*ms,nullptr)==0 && dispatches==3);
 nested=0; assert(darwin_irq_dispatch(0,t0+50*ms,nullptr)==1 && nested==0 && dispatches==4); nested=-1;
 // An edge nobody dispatches after is handed to an idle secondary after 20 ms,
 // again after each interval, and no more once a dispatch has begun.
 int edge[2]; assert(pipe(edge)==0 && wakes==0);
 assert(os_add_epoll_fd(1,edge[0],&token)==0);
 assert(write(edge[1],"c",1)==1);
 assert(wait_for_wake(1) && wait_for_wake(2));
 assert(darwin_irq_dispatch(0,(unsigned long long)os_nsecs(),nullptr)==1);
 sleep_ms(60); int settled=wakes;
 sleep_ms(100); assert(wakes==settled);
 os_close_epoll_fd(); puts("PASS IRQ reentry, MOD rearm, wake source, drain/refill, EOF, early registration, dispatch token, takeover wake, shutdown");
}
