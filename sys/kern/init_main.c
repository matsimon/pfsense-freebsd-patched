/*
 * Copyright (c) 1982, 1986, 1989, 1991, 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)init_main.c	8.9 (Berkeley) 1/21/94
 * $Id: init_main.c,v 1.7 1994/08/27 16:14:25 davidg Exp $
 */

#include <sys/param.h>
#include <sys/filedesc.h>
#include <sys/errno.h>
#include <sys/exec.h>
#include <sys/kernel.h>
#include <sys/mount.h>
#include <sys/map.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/sysent.h>
#include <sys/conf.h>
#include <sys/buf.h>
#include <sys/clist.h>
#include <sys/device.h>
#include <sys/protosw.h>
#include <sys/reboot.h>
#include <sys/user.h>

#include <ufs/ufs/quota.h>

#include <machine/cpu.h>

#include <vm/vm.h>

#ifdef HPFPLIB
char	copyright[] =
"Copyright (c) 1982, 1986, 1989, 1991, 1993\n\tThe Regents of the University of California.\nCopyright (c) 1992 Hewlett-Packard Company\nCopyright (c) 1992 Motorola Inc.\nAll rights reserved.\n\n";
#else
char	copyright[] =
"Copyright (c) 1982, 1986, 1989, 1991, 1993\n\tThe Regents of the University of California.  All rights reserved.\n\n";
#endif

/* Components of the first process -- never freed. */
struct	session session0;
struct	pgrp pgrp0;
struct	proc proc0;
struct	pcred cred0;
struct	filedesc0 filedesc0;
struct	plimit limit0;
struct	vmspace vmspace0;
struct	proc *curproc = &proc0;
struct	proc *initproc, *pageproc, *updateproc;

int	cmask = CMASK;
extern	struct user *proc0paddr;

struct	vnode *rootvp, *swapdev_vp;
int	boothowto;
struct	timeval boottime;
struct	timeval runtime;

static void start_init __P((struct proc *p, void *framep));

#if __GNUC__ >= 2
void __main() {}
#endif

/*
 * This table is filled in by the linker with functions that need to be
 * called to initialize various pseudo-devices and whatnot.
 */

static void dummyinit() {}
TEXT_SET(pseudo_set, dummyinit);

typedef void (*pseudo_func_t)(void);
extern const struct linker_set pseudo_set;
static const pseudo_func_t *pseudos =
	(const pseudo_func_t *)&pseudo_set.ls_items[0];

/*
 * System startup; initialize the world, create process 0, mount root
 * filesystem, and fork to create init and pagedaemon.  Most of the
 * hard work is done in the lower-level initialization routines including
 * startup(), which does memory initialization and autoconfiguration.
 */
void
main(framep)
	void *framep;
{
	register struct proc *p;
	register struct filedesc0 *fdp;
	register struct pdevinit *pdev;
	register int i;
	int s, rval[2];
	extern int (*mountroot) __P((void));
	extern struct pdevinit pdevinit[];
	extern void roundrobin __P((void *));
	extern void schedcpu __P((void *));
	extern struct sysentvec aout_sysvec;

	/*
	 * Initialize the current process pointer (curproc) before
	 * any possible traps/probes to simplify trap processing.
	 */
	p = &proc0;
	curproc = p;
	/*
	 * Attempt to find console and initialize
	 * in case of early panic or other messages.
	 */
	consinit();
	printf(copyright);

	vm_mem_init();
	kmeminit();
	cpu_startup();

	/*
	 * Create process 0 (the swapper).
	 */
	allproc = (volatile struct proc *)p;
	p->p_prev = (struct proc **)&allproc;
	p->p_pgrp = &pgrp0;
	pgrphash[0] = &pgrp0;
	pgrp0.pg_mem = p;
	pgrp0.pg_session = &session0;
	session0.s_count = 1;
	session0.s_leader = p;

	p->p_sysent = &aout_sysvec;

	p->p_flag = P_INMEM | P_SYSTEM;
	p->p_stat = SRUN;
	p->p_nice = NZERO;
	bcopy("swapper", p->p_comm, sizeof ("swapper"));

	/* Create credentials. */
	cred0.p_refcnt = 1;
	p->p_cred = &cred0;
	p->p_ucred = crget();
	p->p_ucred->cr_ngroups = 1;	/* group 0 */

	/* Create the file descriptor table. */
	fdp = &filedesc0;
	p->p_fd = &fdp->fd_fd;
	fdp->fd_fd.fd_refcnt = 1;
	fdp->fd_fd.fd_cmask = cmask;
	fdp->fd_fd.fd_ofiles = fdp->fd_dfiles;
	fdp->fd_fd.fd_ofileflags = fdp->fd_dfileflags;
	fdp->fd_fd.fd_nfiles = NDFILE;

	/* Create the limits structures. */
	p->p_limit = &limit0;
	for (i = 0; i < sizeof(p->p_rlimit)/sizeof(p->p_rlimit[0]); i++)
		limit0.pl_rlimit[i].rlim_cur =
		    limit0.pl_rlimit[i].rlim_max = RLIM_INFINITY;
	limit0.pl_rlimit[RLIMIT_NOFILE].rlim_cur = NOFILE;
	limit0.pl_rlimit[RLIMIT_NPROC].rlim_cur = MAXUPRC;
	i = ptoa(cnt.v_free_count);
	limit0.pl_rlimit[RLIMIT_RSS].rlim_max = i;
	limit0.pl_rlimit[RLIMIT_MEMLOCK].rlim_max = i;
	limit0.pl_rlimit[RLIMIT_MEMLOCK].rlim_cur = i / 3;
	limit0.p_refcnt = 1;

	/* Allocate a prototype map so we have something to fork. */
	p->p_vmspace = &vmspace0;
	vmspace0.vm_refcnt = 1;
	pmap_pinit(&vmspace0.vm_pmap);
	vm_map_init(&vmspace0.vm_map, round_page(VM_MIN_ADDRESS),
	    trunc_page(VM_MAX_ADDRESS), TRUE);
	vmspace0.vm_map.pmap = &vmspace0.vm_pmap;
	p->p_addr = proc0paddr;				/* XXX */

	/*
	 * We continue to place resource usage info and signal
	 * actions in the user struct so they're pageable.
	 */
	p->p_stats = &p->p_addr->u_stats;
	p->p_sigacts = &p->p_addr->u_sigacts;

	/*
	 * Initialize per uid information structure and charge
	 * root for one process.
	 */
	usrinfoinit();
	(void)chgproccnt(0, 1);

	rqinit();

	/* Configure virtual memory system, set vm rlimits. */
	vm_init_limits(p);

	/* Initialize the file systems. */
	vfsinit();

	/* Start real time and statistics clocks. */
	initclocks();

	/* Initialize mbuf's. */
	mbinit();

	/* Initialize clists. */
	clist_init();

	/*
	 * Attach pseudo-devices.
	 */
	while(*pseudos) {
		(**pseudos++)();
	}

	/*
	 * Initialize protocols.  Block reception of incoming packets
	 * until everything is ready.
	 */
	s = splimp();
	ifinit();
	domaininit();
	splx(s);

#ifdef GPROF
	/* Initialize kernel profiling. */
	kmstartup();
#endif

	/* Kick off timeout driven events by calling first time. */
	roundrobin(NULL);
	schedcpu(NULL);

	/* Mount the root file system. */
	if ((*mountroot)())
		panic("cannot mount root");

	/* Get the vnode for '/'.  Set fdp->fd_fd.fd_cdir to reference it. */
	if (VFS_ROOT(mountlist.tqh_first, &rootvnode))
		panic("cannot find root vnode");
	fdp->fd_fd.fd_cdir = rootvnode;
	VREF(fdp->fd_fd.fd_cdir);
	VOP_UNLOCK(rootvnode);
	fdp->fd_fd.fd_rdir = NULL;
	swapinit();

	/*
	 * Now can look at time, having had a chance to verify the time
	 * from the file system.  Reset p->p_rtime as it may have been
	 * munched in mi_switch() after the time got set.
	 */
	p->p_stats->p_start = runtime = mono_time = boottime = time;
	p->p_rtime.tv_sec = p->p_rtime.tv_usec = 0;

	/* Initialize signal state for process 0. */
	siginit(p);

	/* Create process 1 (init(8)). */
	if (fork(p, NULL, rval))
		panic("fork init");
	if (rval[1]) {
		start_init(curproc, framep);
		return;
	}

	/* Create process 2 (the pageout daemon). */
	if (fork(p, NULL, rval))
		panic("fork pager");
	if (rval[1]) {
		/*
		 * Now in process 2.
		 */
		p = curproc;
		pageproc = p;
		p->p_flag |= P_INMEM | P_SYSTEM;	/* XXX */
		bcopy("pagedaemon", curproc->p_comm, sizeof ("pagedaemon"));
		vm_pageout();
		/* NOTREACHED */
	}

	/*
	 * Start update daemon (process 3).
	 */
	if (fork(p, (void *) NULL, rval))
		panic("failed fork update daemon");
	if (rval[1]) {
		p = curproc;
		updateproc = p;
		p->p_flag |= P_INMEM | P_SYSTEM;
		bcopy("update", p->p_comm, sizeof("update"));
		vfs_update();
		/*NOTREACHED*/
	}

	/* The scheduler is an infinite loop. */
	scheduler();
	/* NOTREACHED */
}

/*
 * List of paths to try when searching for "init".
 */
static char *initpaths[] = {
	"/sbin/init",
	"/sbin/oinit",
	"/sbin/init.bak",
	NULL,
};

/*
 * Start the initial user process; try exec'ing each pathname in "initpaths".
 * The program is invoked with one argument containing the boot flags.
 */
static void
start_init(p, framep)
	struct proc *p;
	void *framep;
{
	vm_offset_t addr;
	struct execve_args args;
	int options, i, retval[2], error;
	char **pathp, *path, *ucp, **uap, *arg0, *arg1;

	initproc = p;

	/*
	 * We need to set the system call frame as if we were entered through
	 * a syscall() so that when we call execve() below, it will be able
	 * to set the entry point (see setregs) when it tries to exec.  The
	 * startup code in "locore.s" has allocated space for the frame and
	 * passed a pointer to that space as main's argument.
	 */
	cpu_set_init_frame(p, framep);

	/* XXX */
	p->p_rtprio = RTPRIO_RTOFF;

	/*
	 * Need just enough stack to hold the faked-up "execve()" arguments.
	 */
	addr = trunc_page(VM_MAXUSER_ADDRESS - PAGE_SIZE);
	if (vm_allocate(&p->p_vmspace->vm_map, &addr, PAGE_SIZE, FALSE) != 0)
		panic("init: couldn't allocate argument space");
	p->p_vmspace->vm_maxsaddr = (caddr_t)addr;
	p->p_vmspace->vm_ssize = 1;

	for (pathp = &initpaths[0]; (path = *pathp) != NULL; pathp++) {
		/*
		 * Move out the boot flag argument.
		 */
		options = 0;
		ucp = (char *)USRSTACK;
		(void)subyte(--ucp, 0);		/* trailing zero */
		if (boothowto & RB_SINGLE) {
			(void)subyte(--ucp, 's');
			options = 1;
		}
#ifdef notyet
                if (boothowto & RB_FASTBOOT) {
			(void)subyte(--ucp, 'f');
			options = 1;
		}
#endif
		if (options == 0)
			(void)subyte(--ucp, '-');
		(void)subyte(--ucp, '-');		/* leading hyphen */
		arg1 = ucp;

		/*
		 * Move out the file name (also arg 0).
		 */
		for (i = strlen(path) + 1; i >= 0; i--)
			(void)subyte(--ucp, path[i]);
		arg0 = ucp;

		/*
		 * Move out the arg pointers.
		 */
		uap = (char **)((int)ucp & ~(NBPW-1));
		(void)suword((caddr_t)--uap, 0);	/* terminator */
		(void)suword((caddr_t)--uap, (int)arg1);
		(void)suword((caddr_t)--uap, (int)arg0);

		/*
		 * Point at the arguments.
		 */
		args.fname = arg0;
		args.argv = uap;
		args.envv = NULL;

		/*
		 * Now try to exec the program.  If can't for any reason
		 * other than it doesn't exist, complain.
		 */
		if ((error = execve(p, &args, &retval)) == 0)
			return;
		if (error != ENOENT)
			printf("exec %s: error %d\n", path, error);
	}
	printf("init: not found\n");
	panic("no init");
}
