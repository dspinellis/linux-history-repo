/*
 * List of powerpc syscalls. For the meaning of the _SPU suffix see
 * arch/powerpc/platforms/cell/spu_callbacks.c
 */

SYSCALL(restart_syscall)
SYSCALL(exit)
PPC_SYS(fork)
SYSCALL_SPU(read)
SYSCALL_SPU(write)
COMPAT_SYS_SPU(open)
SYSCALL_SPU(close)
COMPAT_SYS_SPU(waitpid)
COMPAT_SYS_SPU(creat)
SYSCALL_SPU(link)
SYSCALL_SPU(unlink)
COMPAT_SYS(execve)
SYSCALL_SPU(chdir)
COMPAT_SYS_SPU(time)
SYSCALL_SPU(mknod)
SYSCALL_SPU(chmod)
SYSCALL_SPU(lchown)
SYSCALL(ni_syscall)
OLDSYS(stat)
SYSX_SPU(sys_lseek,ppc32_lseek,sys_lseek)
SYSCALL_SPU(getpid)
COMPAT_SYS(mount)
SYSX(sys_ni_syscall,sys_oldumount,sys_oldumount)
SYSCALL_SPU(setuid)
SYSCALL_SPU(getuid)
COMPAT_SYS_SPU(stime)
COMPAT_SYS(ptrace)
SYSCALL_SPU(alarm)
OLDSYS(fstat)
COMPAT_SYS(pause)
COMPAT_SYS(utime)
SYSCALL(ni_syscall)
SYSCALL(ni_syscall)
COMPAT_SYS_SPU(access)
COMPAT_SYS_SPU(nice)
SYSCALL(ni_syscall)
SYSCALL_SPU(sync)
COMPAT_SYS_SPU(kill)
SYSCALL_SPU(rename)
COMPAT_SYS_SPU(mkdir)
SYSCALL_SPU(rmdir)
SYSCALL_SPU(dup)
SYSCALL_SPU(pipe)
COMPAT_SYS_SPU(times)
SYSCALL(ni_syscall)
SYSCALL_SPU(brk)
SYSCALL_SPU(setgid)
SYSCALL_SPU(getgid)
SYSCALL(signal)
SYSCALL_SPU(geteuid)
SYSCALL_SPU(getegid)
SYSCALL(acct)
SYSCALL(umount)
SYSCALL(ni_syscall)
COMPAT_SYS_SPU(ioctl)
COMPAT_SYS_SPU(fcntl)
SYSCALL(ni_syscall)
COMPAT_SYS_SPU(setpgid)
SYSCALL(ni_syscall)
SYSX(sys_ni_syscall,sys_olduname, sys_olduname)
COMPAT_SYS_SPU(umask)
SYSCALL_SPU(chroot)
SYSCALL(ustat)
SYSCALL_SPU(dup2)
SYSCALL_SPU(getppid)
SYSCALL_SPU(getpgrp)
SYSCALL_SPU(setsid)
SYS32ONLY(sigaction)
SYSCALL_SPU(sgetmask)
COMPAT_SYS_SPU(ssetmask)
SYSCALL_SPU(setreuid)
SYSCALL_SPU(setregid)
SYS32ONLY(sigsuspend)
COMPAT_SYS(sigpending)
COMPAT_SYS_SPU(sethostname)
COMPAT_SYS_SPU(setrlimit)
COMPAT_SYS(old_getrlimit)
COMPAT_SYS_SPU(getrusage)
COMPAT_SYS_SPU(gettimeofday)
COMPAT_SYS_SPU(settimeofday)
COMPAT_SYS_SPU(getgroups)
COMPAT_SYS_SPU(setgroups)
SYSX(sys_ni_syscall,sys_ni_syscall,ppc_select)
SYSCALL_SPU(symlink)
OLDSYS(lstat)
COMPAT_SYS_SPU(readlink)
SYSCALL(uselib)
SYSCALL(swapon)
SYSCALL(reboot)
SYSX(sys_ni_syscall,old32_readdir,old_readdir)
SYSCALL_SPU(mmap)
SYSCALL_SPU(munmap)
SYSCALL_SPU(truncate)
SYSCALL_SPU(ftruncate)
SYSCALL_SPU(fchmod)
SYSCALL_SPU(fchown)
COMPAT_SYS_SPU(getpriority)
COMPAT_SYS_SPU(setpriority)
SYSCALL(ni_syscall)
COMPAT_SYS(statfs)
COMPAT_SYS(fstatfs)
SYSCALL(ni_syscall)
COMPAT_SYS_SPU(socketcall)
COMPAT_SYS_SPU(syslog)
COMPAT_SYS_SPU(setitimer)
COMPAT_SYS_SPU(getitimer)
COMPAT_SYS_SPU(newstat)
COMPAT_SYS_SPU(newlstat)
COMPAT_SYS_SPU(newfstat)
SYSX(sys_ni_syscall,sys_uname,sys_uname)
SYSCALL(ni_syscall)
SYSCALL_SPU(vhangup)
SYSCALL(ni_syscall)
SYSCALL(ni_syscall)
COMPAT_SYS_SPU(wait4)
SYSCALL(swapoff)
COMPAT_SYS_SPU(sysinfo)
COMPAT_SYS(ipc)
SYSCALL_SPU(fsync)
SYS32ONLY(sigreturn)
PPC_SYS(clone)
COMPAT_SYS_SPU(setdomainname)
PPC_SYS_SPU(newuname)
SYSCALL(ni_syscall)
COMPAT_SYS_SPU(adjtimex)
SYSCALL_SPU(mprotect)
SYSX(sys_ni_syscall,compat_sys_sigprocmask,sys_sigprocmask)
SYSCALL(ni_syscall)
SYSCALL(init_module)
SYSCALL(delete_module)
SYSCALL(ni_syscall)
SYSCALL(quotactl)
COMPAT_SYS_SPU(getpgid)
SYSCALL_SPU(fchdir)
SYSCALL_SPU(bdflush)
COMPAT_SYS(sysfs)
SYSX_SPU(ppc64_personality,ppc64_personality,sys_personality)
SYSCALL(ni_syscall)
SYSCALL_SPU(setfsuid)
SYSCALL_SPU(setfsgid)
SYSCALL_SPU(llseek)
COMPAT_SYS_SPU(getdents)
SYSX_SPU(sys_select,ppc32_select,ppc_select)
SYSCALL_SPU(flock)
SYSCALL_SPU(msync)
COMPAT_SYS_SPU(readv)
COMPAT_SYS_SPU(writev)
COMPAT_SYS_SPU(getsid)
SYSCALL_SPU(fdatasync)
COMPAT_SYS(sysctl)
SYSCALL_SPU(mlock)
SYSCALL_SPU(munlock)
SYSCALL_SPU(mlockall)
SYSCALL_SPU(munlockall)
COMPAT_SYS_SPU(sched_setparam)
COMPAT_SYS_SPU(sched_getparam)
COMPAT_SYS_SPU(sched_setscheduler)
COMPAT_SYS_SPU(sched_getscheduler)
SYSCALL_SPU(sched_yield)
COMPAT_SYS_SPU(sched_get_priority_max)
COMPAT_SYS_SPU(sched_get_priority_min)
COMPAT_SYS_SPU(sched_rr_get_interval)
COMPAT_SYS_SPU(nanosleep)
SYSCALL_SPU(mremap)
SYSCALL_SPU(setresuid)
SYSCALL_SPU(getresuid)
SYSCALL(ni_syscall)
SYSCALL_SPU(poll)
COMPAT_SYS(nfsservctl)
SYSCALL_SPU(setresgid)
SYSCALL_SPU(getresgid)
COMPAT_SYS_SPU(prctl)
COMPAT_SYS(rt_sigreturn)
COMPAT_SYS(rt_sigaction)
COMPAT_SYS(rt_sigprocmask)
COMPAT_SYS(rt_sigpending)
COMPAT_SYS(rt_sigtimedwait)
COMPAT_SYS(rt_sigqueueinfo)
COMPAT_SYS(rt_sigsuspend)
COMPAT_SYS_SPU(pread64)
COMPAT_SYS_SPU(pwrite64)
SYSCALL_SPU(chown)
SYSCALL_SPU(getcwd)
SYSCALL_SPU(capget)
SYSCALL_SPU(capset)
COMPAT_SYS(sigaltstack)
SYSX_SPU(sys_sendfile64,compat_sys_sendfile,sys_sendfile)
SYSCALL(ni_syscall)
SYSCALL(ni_syscall)
PPC_SYS(vfork)
COMPAT_SYS_SPU(getrlimit)
COMPAT_SYS_SPU(readahead)
SYS32ONLY(mmap2)
SYS32ONLY(truncate64)
SYS32ONLY(ftruncate64)
SYSX(sys_ni_syscall,sys_stat64,sys_stat64)
SYSX(sys_ni_syscall,sys_lstat64,sys_lstat64)
SYSX(sys_ni_syscall,sys_fstat64,sys_fstat64)
SYSCALL(pciconfig_read)
SYSCALL(pciconfig_write)
SYSCALL(pciconfig_iobase)
SYSCALL(ni_syscall)
SYSCALL_SPU(getdents64)
SYSCALL_SPU(pivot_root)
SYSX(sys_ni_syscall,compat_sys_fcntl64,sys_fcntl64)
SYSCALL_SPU(madvise)
SYSCALL_SPU(mincore)
SYSCALL_SPU(gettid)
SYSCALL_SPU(tkill)
SYSCALL_SPU(setxattr)
SYSCALL_SPU(lsetxattr)
SYSCALL_SPU(fsetxattr)
SYSCALL_SPU(getxattr)
SYSCALL_SPU(lgetxattr)
SYSCALL_SPU(fgetxattr)
SYSCALL_SPU(listxattr)
SYSCALL_SPU(llistxattr)
SYSCALL_SPU(flistxattr)
SYSCALL_SPU(removexattr)
SYSCALL_SPU(lremovexattr)
SYSCALL_SPU(fremovexattr)
COMPAT_SYS_SPU(futex)
COMPAT_SYS_SPU(sched_setaffinity)
COMPAT_SYS_SPU(sched_getaffinity)
SYSCALL(ni_syscall)
SYSCALL(ni_syscall)
SYS32ONLY(sendfile64)
COMPAT_SYS_SPU(io_setup)
SYSCALL_SPU(io_destroy)
COMPAT_SYS_SPU(io_getevents)
COMPAT_SYS_SPU(io_submit)
SYSCALL_SPU(io_cancel)
SYSCALL(set_tid_address)
SYSX_SPU(sys_fadvise64,ppc32_fadvise64,sys_fadvise64)
SYSCALL(exit_group)
SYSX(sys_lookup_dcookie,ppc32_lookup_dcookie,sys_lookup_dcookie)
SYSCALL_SPU(epoll_create)
SYSCALL_SPU(epoll_ctl)
SYSCALL_SPU(epoll_wait)
SYSCALL_SPU(remap_file_pages)
SYSX_SPU(sys_timer_create,compat_sys_timer_create,sys_timer_create)
COMPAT_SYS_SPU(timer_settime)
COMPAT_SYS_SPU(timer_gettime)
SYSCALL_SPU(timer_getoverrun)
SYSCALL_SPU(timer_delete)
COMPAT_SYS_SPU(clock_settime)
COMPAT_SYS_SPU(clock_gettime)
COMPAT_SYS_SPU(clock_getres)
COMPAT_SYS_SPU(clock_nanosleep)
SYSX(ppc64_swapcontext,ppc32_swapcontext,ppc_swapcontext)
COMPAT_SYS_SPU(tgkill)
COMPAT_SYS_SPU(utimes)
COMPAT_SYS_SPU(statfs64)
COMPAT_SYS_SPU(fstatfs64)
SYSX(sys_ni_syscall, ppc_fadvise64_64, ppc_fadvise64_64)
PPC_SYS_SPU(rtas)
OLDSYS(debug_setcontext)
SYSCALL(ni_syscall)
COMPAT_SYS(migrate_pages)
COMPAT_SYS(mbind)
COMPAT_SYS(get_mempolicy)
COMPAT_SYS(set_mempolicy)
COMPAT_SYS(mq_open)
SYSCALL(mq_unlink)
COMPAT_SYS(mq_timedsend)
COMPAT_SYS(mq_timedreceive)
COMPAT_SYS(mq_notify)
COMPAT_SYS(mq_getsetattr)
COMPAT_SYS(kexec_load)
COMPAT_SYS(add_key)
COMPAT_SYS(request_key)
COMPAT_SYS(keyctl)
COMPAT_SYS(waitid)
COMPAT_SYS(ioprio_set)
COMPAT_SYS(ioprio_get)
SYSCALL(inotify_init)
SYSCALL(inotify_add_watch)
SYSCALL(inotify_rm_watch)
SYSCALL(spu_run)
SYSCALL(spu_create)
COMPAT_SYS(pselect6)
COMPAT_SYS(ppoll)
SYSCALL_SPU(unshare)
SYSCALL_SPU(splice)
SYSCALL_SPU(tee)
COMPAT_SYS_SPU(vmsplice)
COMPAT_SYS_SPU(openat)
SYSCALL_SPU(mkdirat)
SYSCALL_SPU(mknodat)
SYSCALL_SPU(fchownat)
COMPAT_SYS_SPU(futimesat)
SYSX_SPU(sys_newfstatat, sys_fstatat64, sys_fstatat64)
SYSCALL_SPU(unlinkat)
SYSCALL_SPU(renameat)
SYSCALL_SPU(linkat)
SYSCALL_SPU(symlinkat)
SYSCALL_SPU(readlinkat)
SYSCALL_SPU(fchmodat)
SYSCALL_SPU(faccessat)
COMPAT_SYS_SPU(get_robust_list)
COMPAT_SYS_SPU(set_robust_list)
COMPAT_SYS_SPU(move_pages)
SYSCALL_SPU(getcpu)
COMPAT_SYS(epoll_pwait)
COMPAT_SYS_SPU(utimensat)
COMPAT_SYS_SPU(signalfd)
SYSCALL_SPU(timerfd_create)
SYSCALL_SPU(eventfd)
COMPAT_SYS_SPU(sync_file_range2)
COMPAT_SYS(fallocate)
SYSCALL(subpage_prot)
COMPAT_SYS_SPU(timerfd_settime)
COMPAT_SYS_SPU(timerfd_gettime)
