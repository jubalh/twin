/*
 *  hw.c  --  common functions to use HW/hw_* displays
 *
 *  Copyright (C) 1993-2000 by Massimiliano Ghilardi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 */

/*
 * this is a very sensible part of code, as it must
 * correctly link both against twin and against twdisplay,
 * picking the correct versions of functions like
 * FlushHW(), PanicHW(), AllHWCanDragAreaNow(), DragAreaHW(), etc.
 * 
 * This for example rules out calling methods like Delete()
 * or referencing the variable All.
 */

#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/ttydefaults.h>
#include <sys/resource.h>
#include <sys/wait.h>

#ifndef VDISABLE
# ifdef _POSIX_VDISABLE
#  define VDISABLE	_POSIX_VDISABLE
# else
#  define VDISABLE	255
# endif
#endif

#include "twin.h"

#include "hw.h"
#include "hw_private.h"
#include "common.h"

display_hw *HW, *DisplayHWCTTY;

hwattr *Video, *OldVideo;

byte NeedOldVideo, CanDragArea;
byte ExpensiveFlushVideo, ValidOldVideo, NeedHW;

dat (*ChangedVideo)[2][2];
byte ChangedVideoFlag, ChangedVideoFlagAgain;


udat ScreenWidth = 100, ScreenHeight = 30;

udat CursorX, CursorY;
uldat CursorType;

struct termios ttysave;


static void SignalWinch(int n) {
    if (DisplayHWCTTY && DisplayHWCTTY != HWCTTY_DETACHED
	&& DisplayHWCTTY->DisplayIsCTTY) {
	
	ResizeDisplayPrefer(DisplayHWCTTY);
    }
    signal(SIGWINCH, SignalWinch);
}

static void SignalChild(int n) {
    while (wait4((pid_t)-1, (int *)0, WNOHANG, (struct rusage *)0) > 0)
	;
    signal(SIGCHLD, SignalChild);
}

#ifndef DONT_TRAP_SIGNALS
static void SignalPanic(int n) {
    sigset_t s, t;

    signal(n, SIG_DFL);
    
    sigemptyset(&s);
    sigaddset(&s, n);
    sigprocmask(SIG_BLOCK, &s, &t);
    
    Quit(-n);
    
    kill(getpid(), n);
}
#endif

byte InitSignals(void) {
    signal(SIGWINCH,SignalWinch);
    signal(SIGCHLD, SignalChild);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGIO,   SIG_IGN);
#ifndef DONT_TRAP_SIGNALS
    signal(SIGHUP,  SignalPanic);
    signal(SIGINT,  SignalPanic);
    signal(SIGQUIT, SignalPanic);
    signal(SIGILL,  SignalPanic);
    signal(SIGABRT, SignalPanic);
    signal(SIGBUS,  SignalPanic);
    signal(SIGFPE,  SignalPanic);
    signal(SIGSEGV, SignalPanic);
    signal(SIGTERM, SignalPanic);
    signal(SIGXCPU, SignalPanic);
    signal(SIGXFSZ, SignalPanic);
    signal(SIGPWR,  SignalPanic);
#endif
    return TRUE;
}



void MoveToXY(udat x, udat y) {
    CursorX = x;
    CursorY = y;
}

void SetCursorType(uldat type) {
    if ((type & 0xF) == 0)
	type |= LINECURSOR;
    else if ((type & 0xF) > SOLIDCURSOR)
	type = (type & ~(uldat)0xF) | SOLIDCURSOR;
    
    CursorType = type;
}

void NeedRedrawVideo(udat Left, udat Up, udat Right, udat Down) {
    if (HW->RedrawVideo) {
	HW->RedrawLeft  = Min2(HW->RedrawLeft,  Left);
	HW->RedrawUp    = Min2(HW->RedrawUp,    Up);
	HW->RedrawRight = Max2(HW->RedrawRight, Right);
	HW->RedrawDown  = Max2(HW->RedrawDown,  Down);
    } else {
	HW->RedrawVideo = TRUE;
	HW->RedrawLeft  = Left;
	HW->RedrawUp    = Up;
	HW->RedrawRight = Right;
	HW->RedrawDown  = Down;
    }
}

/*
 * The following functions are quite os-independent,
 * but they are part of the functionality implemented by hw.c
 * so they are included here.
 * 
 * Also, some implementations might hw accel some of these...
 */



/* VideoFlipMouse is quite os-independent ;) */
void VideoFlipMouse(void) {
    uldat pos = (HW->Last_x = HW->MouseState.x) + (HW->Last_y = HW->MouseState.y) * ScreenWidth;
    hwattr h = Video[pos];
    hwcol c = ~HWCOL(h) ^ COL(HIGH,HIGH);

    Video[pos] = HWATTR( c, HWFONT(h) );
}

/*
 * for better cleannes, DirtyVideo()
 * should be used *before* actually touching Video[]
 */
void DirtyVideo(dat Xstart, dat Ystart, dat Xend, dat Yend) {
    dat s0, s1, e0, e1, len, min;
    byte i;
    
    if (Xstart > Xend || Xstart >= ScreenWidth || Xend < 0 ||
	Ystart > Yend || Ystart >= ScreenHeight || Yend < 0)
	return;
    Xstart = Max2(Xstart, 0);
    Ystart = Max2(Ystart, 0);
    Xend = Min2(Xend, ScreenWidth-1);
    Yend = Min2(Yend, ScreenHeight-1);

    ChangedVideoFlag = ChangedVideoFlagAgain = TRUE;
    
    for (; Ystart <= Yend; Ystart++) {
	s0 = ChangedVideo[Ystart][0][0];
	e0 = ChangedVideo[Ystart][0][1];
	s1 = ChangedVideo[Ystart][1][0];
	e1 = ChangedVideo[Ystart][1][1];
	
	/* decide how to rearrange the slots to include Xstart..Xend */
	
	if (s0 == -1) {
	    /* trivial ! */
	    ChangedVideo[Ystart][0][0] = Xstart;
	    ChangedVideo[Ystart][0][1] = Xend;
	    continue;
	}

	if ((s0 != -1 && Xstart >= s0 && Xend <= e0) ||
	    (s1 != -1 && Xstart >= s1 && Xend <= e1))
	    /* nothing to do :) */
	    continue;
	
	
	if (s1 == -1) {
	    /*
	     * this is quite simple.
	     * if s0..e0 and Xstart..Xend intersect or touch, merge.
	     * else just put them in s0,e0 and s1,e1 :
	     * there is no point in forcing a merge now if it costs cells,
	     * we'll merge (if needed) when a third dirty segment appears.
	     */
	    if (Xstart <= e0+1 && Xend+1 >= s0) {
		ChangedVideo[Ystart][0][0] = Min2(s0, Xstart);
		ChangedVideo[Ystart][0][1] = Max2(e0, Xend);
	    } else if (Xstart < s0) {
		ChangedVideo[Ystart][0][0] = Xstart;
		ChangedVideo[Ystart][0][1] = Xend;
		ChangedVideo[Ystart][1][0] = s0;
		ChangedVideo[Ystart][1][1] = e0;
	    } else {
		ChangedVideo[Ystart][1][0] = Xstart;
		ChangedVideo[Ystart][1][1] = Xend;
	    }
	    continue;
	}
	
	/*
	 * now the hairy thing... there are 5 possible actions:
	 * 
	 * 0. put Xstart..Xend in slot 0 and merge s0..e0 with s1..e1
	 * 1. merge Xstart..Xend to s0..e0
	 * 2. merge all together, freeing slot 1
	 * 3. merge Xstart..Xend to s1..e1
	 * 4. put Xstart..Xend in slot 1 and merge s1..e1 with s0..e0
	 *    4. is the same as 0., except for slot inversion.
	 * 
	 * do it the brutal way: calculate the 5 cases and choose
	 * the one resulting in less dirty cells.
	 * 
	 * we do no checks about the relative ordering of s0..e0, s1..e1, Xstart..Xend
	 * and use Min2(sA, sB)..Max2(eA, eB) for merging.
	 * the less dirty pattern automatically selects the best solution
	 * which is non intersecting, and in the 0.,4. cases we choose manually
	 * to preserve ordering between slots.
	 * 
	 * all lenghts are decreased by 2 to avoid always doing (...)+2
	 */
	
	/*0,4*/ min = Xend-Xstart + e1-s0;			i = 0;
	/* 1 */ len = Max2(Xend,e0)-Min2(Xstart,s0) + e1-e0;	if (len <  min) min = len, i = 1;
	/* 2 */	len = Max2(Xend,e1)-Min2(Xstart,s0) - 1;	if (len <= min) min = len, i = 2; /* prefer this if equal */
	/* 3 */	len = e0-s0 + Max2(Xend,e1)-Min2(Xstart,s1);	if (len <  min) min = len, i = 3;
	
	switch (i) {
	  case 0:
	    i = Xstart > s0;
	    ChangedVideo[Ystart][!i][0] = s0;
	    ChangedVideo[Ystart][!i][1] = e1;
	    ChangedVideo[Ystart][ i][0] = Xstart;
	    ChangedVideo[Ystart][ i][1] = Xend;
	    break;
	  case 1:
	    ChangedVideo[Ystart][0][0] = Min2(Xstart,s0);
	    ChangedVideo[Ystart][0][1] = Max2(Xend,e0);
	    break;
	  case 2:
	    ChangedVideo[Ystart][0][0] = Min2(Xstart,s0);
	    ChangedVideo[Ystart][0][1] = Max2(Xend,e1);
	    ChangedVideo[Ystart][1][0] = -1;
	    break;
	  case 3:
	    ChangedVideo[Ystart][1][0] = Min2(Xstart,s1);
	    ChangedVideo[Ystart][1][1] = Max2(Xend,e1);
	    break;
	  default:
	    break;
	}
    }
}

static void Video2OldVideo(dat Xstart, dat Ystart, dat Xend, dat Yend) {
    hwattr *src, *dst;
    uldat xc, yc;
    
    if (Xstart > Xend || Xstart >= ScreenWidth || Xend < 0 ||
	Ystart > Yend || Ystart >= ScreenHeight || Yend < 0)
	return;
    Xstart = Max2(Xstart, 0);
    Ystart = Max2(Ystart, 0);
    Xend = Min2(Xend, ScreenWidth-1);
    Yend = Min2(Yend, ScreenHeight-1);

    yc = Yend - Ystart + 1;
    xc = sizeof(hwattr) * (Xend - Xstart + 1);
    src = Video + Xstart + Ystart * ScreenWidth;
    dst = OldVideo + Xstart + Ystart * ScreenWidth;
    
    while (yc--) {
	CopyMem(src, dst, xc);
	src += ScreenWidth;
	dst += ScreenWidth;
    }
}

/* An important Video function: copy a rectangle. It must be _*FAST*_ !! */
void DragArea(dat Left, dat Up, dat Rgt, dat Dwn, dat DstLeft, dat DstUp) {
    dat DstRgt = DstLeft + (Rgt - Left), DstDwn = DstUp + (Dwn - Up);
    ldat len, count;
    hwattr *src = Video, *dst = Video;
    byte Accel;
    
    count = Dwn - Up + 1;
    len   = (Rgt-Left+1) * sizeof(hwattr);

    /* if HW can do the scroll, use it instead of redrawing */

    /* HACK : for consistency problems, we actually drag only if all HW can drag */
    
    Accel = AllHWCanDragAreaNow(Left, Up, Rgt, Dwn, DstLeft, DstUp);
    
    if (Accel) {
	FlushHW();
	DragAreaHW(Left, Up, Rgt, Dwn, DstLeft, DstUp);
    } else
	DirtyVideo(DstLeft, DstUp, DstRgt, DstDwn);


    /* do the drag inside Video[] */
    
    if (DstUp <= Up) {
	src +=    Left +    Up * ScreenWidth;
	dst += DstLeft + DstUp * ScreenWidth;
	if (DstUp != Up)
	    /* copy forward */
	    while (count--) {
		CopyMem(src, dst, len);
		dst += ScreenWidth;
		src += ScreenWidth;
	    }
	else if (Left != DstLeft)
	    /* the tricky case: DstUp == Up */
	    /* copy forward, but with memmove() */
	    while (count--) {
		MoveMem(src, dst, len);
		dst += ScreenWidth;
		src += ScreenWidth;
	    }
    } else if (DstUp > Up) {
	/* copy backward */
	src +=    Left +    Dwn * ScreenWidth;
	dst += DstLeft + DstDwn * ScreenWidth;
	while (count--) {
	    CopyMem(src, dst, len);
	    dst -= ScreenWidth;
	    src -= ScreenWidth;
	}
    }

    if (Accel && NeedOldVideo)
	Video2OldVideo(DstLeft, DstUp, DstRgt, DstDwn);
}

byte InitTtysave(void) {
    ioctl(0, TCGETS, &ttysave);
    
    ttysave.c_cc [VINTR]	= CINTR;
    ttysave.c_cc [VQUIT]	= CQUIT;
    ttysave.c_cc [VERASE]	= CERASE;
    ttysave.c_cc [VKILL]	= CKILL;
    ttysave.c_cc [VSTART]	= CSTART;
    ttysave.c_cc [VSTOP]	= CSTOP;
    ttysave.c_cc [VSUSP]	= CSUSP;
#ifdef VDSUSP
    ttysave.c_cc [VDSUSP]	= CDSUSP;
#endif
#ifdef VREPRINT
    ttysave.c_cc [VREPRINT]	= CRPRNT;
#endif
#ifdef VDISCRD
    ttysave.c_cc [VDISCRD]	= CFLUSH;
#endif
#ifdef VWERSE
    ttysave.c_cc [VWERSE]	= CWERASE;
#endif
#ifdef VLNEXT
    ttysave.c_cc [VLNEXT]	= CLNEXT;
#endif
	
    ttysave.c_cc [VEOF]	= CEOF;
    ttysave.c_cc [VEOL]	= VDISABLE;
#ifdef VEOL2
    ttysave.c_cc [VEOL2]	= VDISABLE;
#endif
#ifdef VSWTC
    ttysave.c_cc [VSWTC]	= VDISABLE;
#endif
#ifdef VSWTCH
    ttysave.c_cc [VSWTCH]	= VDISABLE;
#endif
    ttysave.c_cc [VMIN]	= 1;
    ttysave.c_cc [VTIME]	= 0;
	
    /* input modes */
    ttysave.c_iflag = (BRKINT | IGNPAR | ICRNL | IXON
#ifdef IMAXBEL
		       | IMAXBEL
#endif
		       );
	
    /* output modes */
    ttysave.c_oflag = (OPOST | ONLCR);
    
    /* control modes */
    ttysave.c_cflag = (CS8 | CREAD);
	
    /* line discipline modes */
    ttysave.c_lflag = (ISIG | ICANON | IEXTEN | ECHO | ECHOE | ECHOK
#ifdef ECHOKE
		       | ECHOKE
#endif
		       );
    
    return TRUE;
}
