/**
 * Picomimi-x64 — XNU/Darwin Compatibility Layer
 *
 * This header defines everything the XNU compat subsystem needs:
 *
 *   1. Process personality flags (additive — Linux personality unchanged)
 *   2. Mach port handle types (lightweight fake port table)
 *   3. XNU BSD syscall numbers (macOS 10.15+ / 0x2000000 class)
 *   4. Mach trap numbers      (macOS 0x1000000 class)
 *   5. errno translation table (Darwin ↔ POSIX)
 *   6. Darwin stat/statfs struct layouts (differ from Linux)
 *   7. kevent / kqueue types
 *   8. Public API used by exec.c and syscall_table.c
 *
 * RULES:
 *   - Linux syscall table is NEVER modified.
 *   - Personality is per-task; kernel threads are always PERSONALITY_LINUX.
 *   - All shims call existing sys_* functions — no duplicated logic.
 */

#ifndef _KERNEL_XNU_COMPAT_H
#define _KERNEL_XNU_COMPAT_H

#include <kernel/types.h>

/* =========================================================
 * 1.  Process personality
 * ========================================================= */

#define PERSONALITY_LINUX   0x00    /* default — Linux ABI (unchanged)    */
#define PERSONALITY_XNU     0x01    /* macOS/XNU userspace binary          */

/* personality lives in task_struct.personality (u8) */

/* =========================================================
 * 2.  Mach port handles
 *
 * Real XNU has a full port right / IPC space mechanism.
 * We use a minimal fake: a small per-process array of 32 slots.
 * Port names are u32 values 1..32 (0 = MACH_PORT_NULL).
 * Each slot records what the port refers to (task, thread, semaphore…).
 *
 * This is enough to make mach_task_self(), mach_thread_self(),
 * semaphore_create/wait/signal and basic vm_ traps work.
 * ========================================================= */

#define MACH_PORT_NULL          ((u32)0)
#define MACH_PORT_DEAD          ((u32)0xFFFFFFFFu)
#define MACH_PORT_MAX_SLOTS     32

typedef u32 mach_port_t;
typedef u32 mach_port_name_t;

typedef enum mach_port_type {
    MACH_PORT_TYPE_NONE = 0,
    MACH_PORT_TYPE_TASK,
    MACH_PORT_TYPE_THREAD,
    MACH_PORT_TYPE_SEMAPHORE,
    MACH_PORT_TYPE_HOST,
} mach_port_type_t;

typedef struct mach_port_slot {
    mach_port_type_t    type;
    void               *object;     /* task_struct_t* / semaphore_t* / NULL */
    u32                 refs;
} mach_port_slot_t;

/* Well-known bootstrap port names (fixed values mirroring XNU conventions) */
#define MACH_PORT_NAME_TASK_SELF    ((mach_port_t)1)
#define MACH_PORT_NAME_THREAD_SELF  ((mach_port_t)2)
#define MACH_PORT_NAME_HOST         ((mach_port_t)3)
#define MACH_PORT_NAME_FIRST_FREE   ((mach_port_t)4)

/* =========================================================
 * 3.  XNU BSD syscall class  (sysnum | 0x2000000)
 *
 * Only the numbers that differ meaningfully from Linux are listed.
 * For ones that map 1:1 or near-1:1 the shim table handles them.
 * ========================================================= */

#define XNU_BSD_CLASS       0x2000000U
#define XNU_MACH_CLASS      0x1000000U

/* Decode class from raw rax */
#define XNU_SYSCALL_CLASS(n)    ((n) >> 24)
#define XNU_SYSCALL_NUMBER(n)   ((n) & 0x00FFFFFFU)
#define XNU_CLASS_BSD           2
#define XNU_CLASS_MACH          1
#define XNU_CLASS_MDEP          3   /* machine-dependent */

/* Selected BSD syscall numbers (Darwin 64-bit) */
#define XNU_NR_syscall          0
#define XNU_NR_exit             1
#define XNU_NR_fork             2
#define XNU_NR_read             3
#define XNU_NR_write            4
#define XNU_NR_open             5
#define XNU_NR_close            6
#define XNU_NR_wait4            7
#define XNU_NR_link             9
#define XNU_NR_unlink           10
#define XNU_NR_chdir            12
#define XNU_NR_fchdir           13
#define XNU_NR_mknod            14
#define XNU_NR_chmod            15
#define XNU_NR_chown            16
#define XNU_NR_getpid           20
#define XNU_NR_setuid           23
#define XNU_NR_getuid           24
#define XNU_NR_geteuid          25
#define XNU_NR_recvmsg          27
#define XNU_NR_sendmsg          28
#define XNU_NR_recvfrom         29
#define XNU_NR_accept           30
#define XNU_NR_getpeername      31
#define XNU_NR_getsockname      32
#define XNU_NR_access           33
#define XNU_NR_chflags          34
#define XNU_NR_fchflags         35
#define XNU_NR_sync             36
#define XNU_NR_kill             37
#define XNU_NR_getppid          39
#define XNU_NR_dup              41
#define XNU_NR_pipe             42
#define XNU_NR_getegid          43
#define XNU_NR_sigaction        46
#define XNU_NR_getgid           47
#define XNU_NR_sigprocmask      48
#define XNU_NR_getlogin         49
#define XNU_NR_setlogin         50
#define XNU_NR_ioctl            54
#define XNU_NR_revoke           56
#define XNU_NR_symlink          57
#define XNU_NR_readlink         58
#define XNU_NR_execve           59
#define XNU_NR_umask            60
#define XNU_NR_chroot           61
#define XNU_NR_msync            65
#define XNU_NR_vfork            66
#define XNU_NR_munmap           73
#define XNU_NR_mprotect         74
#define XNU_NR_madvise          75
#define XNU_NR_mincore          78
#define XNU_NR_getgroups        79
#define XNU_NR_setgroups        80
#define XNU_NR_getpgrp          81
#define XNU_NR_setpgid          82
#define XNU_NR_setitimer        83
#define XNU_NR_getitimer        86
#define XNU_NR_dup2             90
#define XNU_NR_fcntl            92
#define XNU_NR_select           93
#define XNU_NR_fsync            95
#define XNU_NR_setpriority      96
#define XNU_NR_socket           97
#define XNU_NR_connect          98
#define XNU_NR_getpriority      100
#define XNU_NR_bind             104
#define XNU_NR_setsockopt       105
#define XNU_NR_listen           106
#define XNU_NR_sigsuspend       111
#define XNU_NR_gettimeofday     116
#define XNU_NR_getrusage        117
#define XNU_NR_getsockopt       118
#define XNU_NR_readv            120
#define XNU_NR_writev           121
#define XNU_NR_settimeofday     122
#define XNU_NR_fchown           123
#define XNU_NR_fchmod           124
#define XNU_NR_rename           128
#define XNU_NR_flock            131
#define XNU_NR_mkfifo           132
#define XNU_NR_sendto           133
#define XNU_NR_shutdown         134
#define XNU_NR_socketpair       135
#define XNU_NR_mkdir            136
#define XNU_NR_rmdir            137
#define XNU_NR_utimes           138
#define XNU_NR_futimes          139
#define XNU_NR_gethostuuid      142
#define XNU_NR_setsid           147
#define XNU_NR_getpgid          151
#define XNU_NR_setprivexec      152
#define XNU_NR_pread            153
#define XNU_NR_pwrite           154
#define XNU_NR_nfssvc           155
#define XNU_NR_statfs           157
#define XNU_NR_fstatfs          158
#define XNU_NR_getfh            161
#define XNU_NR_getdomainname    162
#define XNU_NR_setdomainname    163
#define XNU_NR_quotactl         165
#define XNU_NR_mount            167
#define XNU_NR_csops            169
#define XNU_NR_csops_audittoken 170
#define XNU_NR_waitid           173
#define XNU_NR_kdebug_typefilter 177
#define XNU_NR_kdebug_trace_string 178
#define XNU_NR_kdebug_trace64   179
#define XNU_NR_kdebug_trace     180
#define XNU_NR_setgid           181
#define XNU_NR_setegid          182
#define XNU_NR_seteuid          183
#define XNU_NR_sigreturn        184
#define XNU_NR_thread_selfcounts 186
#define XNU_NR_fdatasync        187
#define XNU_NR_stat             188   /* stat64 on Darwin */
#define XNU_NR_fstat            189
#define XNU_NR_lstat            190
#define XNU_NR_pathconf         191
#define XNU_NR_fpathconf        192
#define XNU_NR_getrlimit        194
#define XNU_NR_setrlimit        195
#define XNU_NR_getdirentries    196
#define XNU_NR_mmap             197
#define XNU_NR_lseek            199
#define XNU_NR_truncate         200
#define XNU_NR_ftruncate        201
#define XNU_NR_sysctl           202
#define XNU_NR_mlock            203
#define XNU_NR_munlock          204
#define XNU_NR_undelete         205
#define XNU_NR_open_dprotected_np  216
#define XNU_NR_getattrlist      220
#define XNU_NR_getdirentriesattr 222
#define XNU_NR_exchangedata     223
#define XNU_NR_searchfs         225
#define XNU_NR_delete           226
#define XNU_NR_copyfile         227
#define XNU_NR_fgetattrlist     228
#define XNU_NR_fsetattrlist     229
#define XNU_NR_poll             230
#define XNU_NR_getxattr         234
#define XNU_NR_fgetxattr        235
#define XNU_NR_setxattr         236
#define XNU_NR_fsetxattr        237
#define XNU_NR_removexattr      238
#define XNU_NR_fremovexattr     239
#define XNU_NR_listxattr        240
#define XNU_NR_flistxattr       241
#define XNU_NR_fsctl            242
#define XNU_NR_initgroups       243
#define XNU_NR_posix_spawn      244
#define XNU_NR_ffsctl           245
#define XNU_NR_nfsclnt          247
#define XNU_NR_fhopen           248
#define XNU_NR_minherit         250
#define XNU_NR_semsys           251
#define XNU_NR_msgsys           252
#define XNU_NR_shmsys           253
#define XNU_NR_semctl           254
#define XNU_NR_semget           255
#define XNU_NR_semop            256
#define XNU_NR_msgctl           258
#define XNU_NR_msgget           259
#define XNU_NR_msgsnd           260
#define XNU_NR_msgrcv           261
#define XNU_NR_shmat            262
#define XNU_NR_shmctl           263
#define XNU_NR_shmdt            264
#define XNU_NR_shmget           265
#define XNU_NR_shm_open         266
#define XNU_NR_shm_unlink       267
#define XNU_NR_sem_open         268
#define XNU_NR_sem_close        269
#define XNU_NR_sem_unlink       270
#define XNU_NR_sem_wait         271
#define XNU_NR_sem_trywait      272
#define XNU_NR_sem_post         273
#define XNU_NR_sysctlbyname     274
#define XNU_NR_open_extended    277
#define XNU_NR_umask_extended   278
#define XNU_NR_stat_extended    279
#define XNU_NR_lstat_extended   280
#define XNU_NR_fstat_extended   281
#define XNU_NR_chmod_extended   282
#define XNU_NR_fchmod_extended  283
#define XNU_NR_access_extended  284
#define XNU_NR_settid           285
#define XNU_NR_gettid           286
#define XNU_NR_setsgroups       287
#define XNU_NR_getsgroups       288
#define XNU_NR_setwgroups       289
#define XNU_NR_getwgroups       290
#define XNU_NR_mkfifo_extended  291
#define XNU_NR_mkdir_extended   292
#define XNU_NR_identitysvc      293
#define XNU_NR_shared_region_check_np 294
#define XNU_NR_vm_pressure_monitor 296
#define XNU_NR_psynch_rw_longrdlock 297
#define XNU_NR_psynch_rw_yieldwrlock 298
#define XNU_NR_psynch_rw_downgrade 299
#define XNU_NR_psynch_rw_upgrade 300
#define XNU_NR_psynch_mutexwait  301
#define XNU_NR_psynch_mutexdrop  302
#define XNU_NR_psynch_cvbroad    303
#define XNU_NR_psynch_cvsignal   304
#define XNU_NR_psynch_cvwait     305
#define XNU_NR_psynch_rw_rdlock  306
#define XNU_NR_psynch_rw_wrlock  307
#define XNU_NR_psynch_rw_unlock  308
#define XNU_NR_psynch_rw_unlock2 309
#define XNU_NR_getsid            310
#define XNU_NR_settid_with_pid   311
#define XNU_NR_psynch_cvclrprepost 312
#define XNU_NR_aio_fsync         313
#define XNU_NR_aio_return        314
#define XNU_NR_aio_suspend       315
#define XNU_NR_aio_cancel        316
#define XNU_NR_aio_error         317
#define XNU_NR_aio_read          318
#define XNU_NR_aio_write         319
#define XNU_NR_lio_listio        320
#define XNU_NR_iopolicysys       322
#define XNU_NR_process_policy    323
#define XNU_NR_mlockall          324
#define XNU_NR_munlockall        325
#define XNU_NR_issetugid         327
#define XNU_NR___pthread_kill    328
#define XNU_NR___pthread_sigmask 329
#define XNU_NR___sigwait         330
#define XNU_NR___disable_threadsignal 331
#define XNU_NR___pthread_markcancel 332
#define XNU_NR___pthread_canceled 333
#define XNU_NR___semwait_signal  334
#define XNU_NR_proc_info         336
#define XNU_NR_sendfile          337
#define XNU_NR_stat64            338
#define XNU_NR_fstat64           339
#define XNU_NR_lstat64           340
#define XNU_NR_stat64_extended   341
#define XNU_NR_lstat64_extended  342
#define XNU_NR_fstat64_extended  343
#define XNU_NR_getdirentries64   344
#define XNU_NR_statfs64          345
#define XNU_NR_fstatfs64         346
#define XNU_NR_getfsstat64       347
#define XNU_NR___pthread_chdir   348
#define XNU_NR___pthread_fchdir  349
#define XNU_NR_audit             350
#define XNU_NR_auditon           351
#define XNU_NR_getauid           353
#define XNU_NR_setauid           354
#define XNU_NR_getaudit_addr     357
#define XNU_NR_setaudit_addr     358
#define XNU_NR_auditctl          359
#define XNU_NR_bsdthread_create  360
#define XNU_NR_bsdthread_terminate 361
#define XNU_NR_kqueue            362
#define XNU_NR_kevent            363
#define XNU_NR_lchown            364
#define XNU_NR_bsdthread_register 366
#define XNU_NR_workq_open        367
#define XNU_NR_workq_kernreturn  368
#define XNU_NR_kevent64          369
#define XNU_NR___old_semwait_signal 370
#define XNU_NR___old_semwait_signal_nocancel 371
#define XNU_NR_thread_selfid     372
#define XNU_NR_ledger            373
#define XNU_NR_kevent_qos        374
#define XNU_NR_kevent_id         375
#define XNU_NR___mac_execve      380
#define XNU_NR___mac_syscall     381
#define XNU_NR___mac_get_file    382
#define XNU_NR___mac_set_file    383
#define XNU_NR___mac_get_link    384
#define XNU_NR___mac_set_link    385
#define XNU_NR___mac_get_proc    386
#define XNU_NR___mac_set_proc    387
#define XNU_NR___mac_get_fd      388
#define XNU_NR___mac_set_fd      389
#define XNU_NR___mac_get_pid     390
#define XNU_NR_pselect           394
#define XNU_NR_pselect_nocancel  395
#define XNU_NR_read_nocancel     396
#define XNU_NR_write_nocancel    397
#define XNU_NR_open_nocancel     398
#define XNU_NR_close_nocancel    399
#define XNU_NR_wait4_nocancel    400
#define XNU_NR_recvmsg_nocancel  401
#define XNU_NR_sendmsg_nocancel  402
#define XNU_NR_recvfrom_nocancel 403
#define XNU_NR_accept_nocancel   404
#define XNU_NR_msync_nocancel    405
#define XNU_NR_fcntl_nocancel    406
#define XNU_NR_select_nocancel   407
#define XNU_NR_fsync_nocancel    408
#define XNU_NR_connect_nocancel  409
#define XNU_NR_sigsuspend_nocancel 410
#define XNU_NR_readv_nocancel    411
#define XNU_NR_writev_nocancel   412
#define XNU_NR_sendto_nocancel   413
#define XNU_NR_pread_nocancel    414
#define XNU_NR_pwrite_nocancel   415
#define XNU_NR_waitid_nocancel   416
#define XNU_NR_poll_nocancel     417
#define XNU_NR_msgsnd_nocancel   418
#define XNU_NR_msgrcv_nocancel   419
#define XNU_NR_sem_wait_nocancel 420
#define XNU_NR_aio_suspend_nocancel 421
#define XNU_NR___sigwait_nocancel 422
#define XNU_NR___semwait_signal_nocancel 423
#define XNU_NR_mac_mount         424
#define XNU_NR_mac_get_mount     425
#define XNU_NR_mac_getfsstat     426
#define XNU_NR_fsgetpath         427
#define XNU_NR_audit_session_self 428
#define XNU_NR_audit_session_join 429
#define XNU_NR_fileport_makeport 430
#define XNU_NR_fileport_makefd   431
#define XNU_NR_audit_session_port 432
#define XNU_NR_pid_suspend       433
#define XNU_NR_pid_resume        434
#define XNU_NR_pid_hibernate     435
#define XNU_NR_pid_shutdown_sockets 436
#define XNU_NR_shared_region_map_and_slide_np 438
#define XNU_NR_kas_info          439
#define XNU_NR_memorystatus_control 440
#define XNU_NR_guarded_open_np   441
#define XNU_NR_guarded_close_np  442
#define XNU_NR_guarded_kqueue_np 443
#define XNU_NR_change_fdguard_np 444
#define XNU_NR_usrctl            445
#define XNU_NR_proc_rlimit_control 446
#define XNU_NR_openat            447
#define XNU_NR_openat_nocancel   448
#define XNU_NR_renameat          449
#define XNU_NR_faccessat         450
#define XNU_NR_fchmodat          451
#define XNU_NR_fchownat          452
#define XNU_NR_fstatat           453
#define XNU_NR_fstatat64         454
#define XNU_NR_linkat            455
#define XNU_NR_unlinkat          456
#define XNU_NR_readlinkat        457
#define XNU_NR_symlinkat         458
#define XNU_NR_mkdirat           459
#define XNU_NR_getattrlistat     460
#define XNU_NR_proc_trace_log    461
#define XNU_NR_bsdthread_ctl     478
#define XNU_NR_openbyid_np       479
#define XNU_NR_recvmsg_x         480
#define XNU_NR_sendmsg_x         481
#define XNU_NR_thread_selfusage  482
#define XNU_NR_csrctl            483
#define XNU_NR_guarded_open_dprotected_np 484
#define XNU_NR_guarded_write_np  485
#define XNU_NR_guarded_pwrite_np 486
#define XNU_NR_guarded_writev_np 487
#define XNU_NR_renameatx_np      488
#define XNU_NR_mremap_encrypted  489
#define XNU_NR_netagent_trigger  490
#define XNU_NR_stack_snapshot_with_config 491
#define XNU_NR_microstackshot    492
#define XNU_NR_grab_pgo_data     493
#define XNU_NR_persona           494
#define XNU_NR_mach_eventlink_signal 523
#define XNU_NR_mach_eventlink_wait_until 524
#define XNU_NR_mach_eventlink_signal_wait_until 525
#define XNU_NR_work_interval_ctl 526
#define XNU_NR_getentropy        527
#define XNU_NR_necp_open         528
#define XNU_NR_necp_client_action 529
#define XNU_NR___nexus_open      540
#define XNU_NR_clonefileat       582
#define XNU_NR_fclonefileat      583

#define XNU_NR_BSD_MAX           600

/* =========================================================
 * 4.  Mach trap numbers  (rax | 0x1000000)
 * ========================================================= */

#define MACH_NR_kern_invalid        0
#define MACH_NR_reply_port         26
#define MACH_NR_thread_self_trap   27
#define MACH_NR_task_self_trap     28
#define MACH_NR_host_self_trap     29
#define MACH_NR_mach_msg_trap      31
#define MACH_NR_mach_msg_overwrite_trap 32
#define MACH_NR_semaphore_signal_trap   33
#define MACH_NR_semaphore_signal_all_trap 34
#define MACH_NR_semaphore_signal_thread_trap 35
#define MACH_NR_semaphore_wait_trap     36
#define MACH_NR_semaphore_wait_signal_trap 37
#define MACH_NR_semaphore_timedwait_trap 38
#define MACH_NR_semaphore_timedwait_signal_trap 39
#define MACH_NR_task_name_for_pid  45
#define MACH_NR_task_for_pid       45
#define MACH_NR_pid_for_task       46
#define MACH_NR_mach_msg2_trap     70
#define MACH_NR_macx_swapon       48
#define MACH_NR_macx_swapoff      49
#define MACH_NR_macx_triggers     51
#define MACH_NR_macx_backing_store_suspend 52
#define MACH_NR_macx_backing_store_recovery 53
#define MACH_NR_swtch_pri         59
#define MACH_NR_swtch             60
#define MACH_NR_thread_switch     61
#define MACH_NR_clock_sleep_trap  62
#define MACH_NR_mach_timebase_info 89
#define MACH_NR_mach_wait_until   90
#define MACH_NR_mk_timer_create   91
#define MACH_NR_mk_timer_destroy  92
#define MACH_NR_mk_timer_arm      93
#define MACH_NR_mk_timer_cancel   94
#define MACH_NR_iokit_user_client_trap 100

#define MACH_NR_MAX               128

/* =========================================================
 * 5.  Errno translation: Darwin → Linux (negative errno)
 *
 * Most Darwin errnos match POSIX (same as Linux), but a handful differ.
 * xnu_errno(linux_err) → Darwin errno (positive)
 * ========================================================= */

/* Darwin-specific errnos that differ from Linux */
#define DARWIN_ENOTSUP          45
#define DARWIN_EPROCLIM         67
#define DARWIN_EUSERS           68
#define DARWIN_EDQUOT           69
#define DARWIN_ESTALE           70
#define DARWIN_EBADRPC          72
#define DARWIN_ERPCMISMATCH     73
#define DARWIN_EPROGUNAVAIL     74
#define DARWIN_EPROGMISMATCH    75
#define DARWIN_EPROCUNAVAIL     76
#define DARWIN_EFTYPE           79
#define DARWIN_EAUTH            80
#define DARWIN_ENEEDAUTH        81
#define DARWIN_EPWROFF          82
#define DARWIN_EDEVERR          83
#define DARWIN_EOVERFLOW        84
#define DARWIN_EBADEXEC         85
#define DARWIN_EBADARCH         86
#define DARWIN_ESHLIBVERS       87
#define DARWIN_EBADMACHO        88
#define DARWIN_ECANCELED        89
#define DARWIN_EIDRM            90
#define DARWIN_ENOMSG           91
#define DARWIN_EILSEQ           92
#define DARWIN_ENOATTR          93
#define DARWIN_EBADMSG          94
#define DARWIN_EMULTIHOP        95
#define DARWIN_ENODATA          96
#define DARWIN_ENOLINK          97
#define DARWIN_ENOSR            98
#define DARWIN_ENOSTR           99
#define DARWIN_EPROTO          100
#define DARWIN_ETIME           101
#define DARWIN_EOPNOTSUPP      102
#define DARWIN_ENOPOLICY       103
#define DARWIN_ENOTRECOVERABLE 104
#define DARWIN_EOWNERDEAD      105
#define DARWIN_EQFULL          106
#define DARWIN_EAFNOSUPPORT    47

s64 xnu_errno(s64 linux_err);

/* =========================================================
 * 6.  Darwin stat64 struct  (differs from Linux stat)
 * ========================================================= */

typedef struct darwin_stat64 {
    u32     st_dev;
    u16     st_mode;
    u16     st_nlink;
    u64     st_ino;
    u32     st_uid;
    u32     st_gid;
    u32     st_rdev;
    /* timespec: tv_sec (i64) + tv_nsec (i64) */
    s64     st_atimespec_sec;
    s64     st_atimespec_nsec;
    s64     st_mtimespec_sec;
    s64     st_mtimespec_nsec;
    s64     st_ctimespec_sec;
    s64     st_ctimespec_nsec;
    s64     st_birthtimespec_sec;
    s64     st_birthtimespec_nsec;
    s64     st_size;
    s64     st_blocks;
    u32     st_blksize;
    u32     st_flags;
    u32     st_gen;
    s32     st_lspare;
    s64     st_qspare[2];
} darwin_stat64_t;

/* =========================================================
 * 7.  kqueue / kevent  (Darwin's epoll equivalent)
 * ========================================================= */

/* kevent filters */
#define EVFILT_READ         (-1)
#define EVFILT_WRITE        (-2)
#define EVFILT_AIO          (-3)
#define EVFILT_VNODE        (-4)
#define EVFILT_PROC         (-5)
#define EVFILT_SIGNAL       (-6)
#define EVFILT_TIMER        (-7)
#define EVFILT_MACHPORT     (-8)
#define EVFILT_FS           (-9)
#define EVFILT_USER         (-10)
#define EVFILT_VM           (-12)
#define EVFILT_SYSCOUNT     13

/* kevent flags */
#define EV_ADD      0x0001
#define EV_DELETE   0x0002
#define EV_ENABLE   0x0004
#define EV_DISABLE  0x0008
#define EV_ONESHOT  0x0010
#define EV_CLEAR    0x0020
#define EV_RECEIPT  0x0040
#define EV_DISPATCH 0x0080
#define EV_UDATA_SPECIFIC 0x0100
#define EV_EOF      0x8000
#define EV_ERROR    0x4000
#define EV_FLAG0    0x1000
#define EV_FLAG1    0x2000

/* kevent64_s — the 64-bit kevent structure */
typedef struct kevent64_s {
    u64     ident;      /* identifier for this event */
    s16     filter;     /* filter for event */
    u16     flags;      /* general flags */
    u32     fflags;     /* filter-specific flags */
    s64     data;       /* filter-specific data */
    u64     udata;      /* opaque user data identifier */
    u64     ext[2];     /* filter-specific extensions */
} kevent64_t;

/* kevent (original, 32-bit ident) */
typedef struct kevent {
    u64     ident;
    s16     filter;
    u16     flags;
    u32     fflags;
    s64     data;
    void   *udata;
} kevent_t;

/* kqueue fd internal state */
#define KQUEUE_MAX_EVENTS   256

typedef struct kqueue_state {
    kevent64_t  registered[KQUEUE_MAX_EVENTS];
    int         nregistered;
    spinlock_t  lock;
} kqueue_state_t;

/* =========================================================
 * 8.  Public API
 * ========================================================= */

/* BSD shim dispatch */
s64 xnu_bsd_dispatch(u64 nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6);

/* Mach trap dispatch */
s64 xnu_mach_dispatch(u64 nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6);

/* Top-level XNU syscall entry (called from syscall_dispatch) */
s64 xnu_syscall_dispatch(u64 raw_nr, u64 a1, u64 a2, u64 a3,
                          u64 a4, u64 a5, u64 a6);

/* Mach port helpers */
mach_port_t mach_port_alloc_task(void);   /* allocate TASK_SELF port for current */
mach_port_t mach_port_alloc_thread(void); /* allocate THREAD_SELF port */
mach_port_t mach_port_alloc_host(void);   /* allocate HOST_SELF port */
void        mach_ports_init_task(void);    /* called on exec for XNU tasks */

/* kqueue helpers */
int  kqueue_create(void);
int  kqueue_kevent(int kqfd, const kevent_t *changelist, int nchanges,
                   kevent_t *eventlist, int nevents,
                   const struct timespec *timeout);

#endif /* _KERNEL_XNU_COMPAT_H */
