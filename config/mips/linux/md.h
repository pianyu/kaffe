/*
 * mips/linux/md.h
 * Linux MIPS configuration information.
 *
 * Copyright (c) 1996, 1997, 1998
 *	Transvirtual Technologies, Inc.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution 
 * of this file. 
 */

#ifndef __mips_linux_md_h
#define __mips_linux_md_h

#include "mips/common.h"
#include "mips/threads.h"

#undef	SP_OFFSET
#undef	FP_OFFSET

#define	SP_OFFSET	1
#define	FP_OFFSET	10

#if defined(TRANSLATOR)
#include "jit-md.h"
#endif

/**/
/* Signal handling */
/**/
#include <sigcontext.h>

/* Define the entry into a signal handler */
#define EXCEPTIONPROTO  int sig, int c1, int c2, int c3, int c4, int c5, int c6, int c7, struct sigcontext ctx

/* Get the first exception frame from a signal handler */
#define MIPS_FP 30
#define EXCEPTIONFRAME(f, c) \
	(f).return_frame = (void*)(c).sc_regs[MIPS_FP]; \
	(f).return_pc = (void*)(c).sc_pc


#endif
