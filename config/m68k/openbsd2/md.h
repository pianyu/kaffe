/*
 * m68k/netbsd/md.h
 * netbsd/m68k specific functions.
 *
 * Copyright (c) 1996 Matthias Hopf <mshopf@informatik.uni-erlangen.de>
 * netbsd/m68k version  Erik Johannessen <erjohann@sn.no>
 *
 * Permission granted for Tim Wilkinson to include this source in his
 * Kaffe system.
 *
 * Copyright (c) 1996, 1997
 *	Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution 
 * of this file. 
 */

#ifndef __m68k_openbsd_md_h
#define __m68k_openbsd_md_h

#include "m68k/common.h"
#include "m68k/threads.h"
#include "m68k/sysdepCallMethod.h"

/*
 * Stack offset.
 * This is the offset into the setjmp buffer 
 * where the stack pointer is stored.
 */
#undef	SP_OFFSET
#define	SP_OFFSET		2

/**/
/* Extra exception handling information. */
/**/
#include <signal.h>

#define	SIGNAL_ARGS(sig, sc) int sig, siginfo_t* code, struct sigcontext *sc
#define SIGNAL_CONTEXT_POINTER(scp) struct sigcontext *scp
#define GET_SIGNAL_CONTEXT_POINTER(scp) (scp)
#define SIGNAL_PC(scp) ((uintp)(scp)->sc_pc)

#include "kaffe-unix-stack.h"

#if defined(TRANSLATOR)
#include "jit-md.h"
#endif

#endif
