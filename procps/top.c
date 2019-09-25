/* top.c - Source file:         show Linux processes */
/*
 * Copyright (c) 2002, by:      James C. Warner
 *    All rights reserved.      8921 Hilloway Road
 *                              Eden Prairie, Minnesota 55347 USA
 *                             <warnerjc@worldnet.att.net>
 *
 * This file may be used subject to the terms and conditions of the
 * GNU Library General Public License Version 2, or any later version
 * at your option, as published by the Free Software Foundation.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 */
/* For their contributions to this program, the author wishes to thank:
 *    Craig Small, <csmall@small.dropbear.id.au>
 *    Albert D. Cahalan, <albert@users.sf.net>
 */
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <ctype.h>
#include <curses.h>
#include <errno.h>
#ifndef YIELDCPU_OFF
#include <sched.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <values.h>
   /*
      I am listing precisely why each header is needed because of the
      construction of libproc -- separate header files may not always be
      available and function names are not normalized.  We have avoided
      some library routine(s) as overkill and have subsumed some others.
   */
        /* need: 1 define + dev_to_tty */
#include "proc/devname.h"
        /* need: (ksym.c) open_psdb_message, wchan, close_psdb (redhat only) */
#include "proc/procps.h"
        /* need: 2 types + openproc, readproc, closeproc */
#include "proc/readproc.h"
        /* need: signal_name_to_number */
#include "proc/sig.h"
#ifdef USE_LIB_STA3
        /* need: status */
#include "proc/status.h"
#endif
        /* need: meminfo stuff */
#include "proc/sysinfo.h"
        /* need: procps_version + kernel version stuff */
#include "proc/version.h"
        /* need: sprint_uptime */
#include "proc/whattime.h"

#include "top.h"

/*######  Miscellaneous global stuff  ####################################*/

        /* The original and new terminal attributes */
static struct termios Savedtty,
                      Rawtty;
static int  Ttychanged = 0;

        /* Program name used in error messages and local 'rc' file name */
static char *Myname;

        /* The Name of the local config file, dynamically constructed */
static char  RCfile [OURPATHSZ];
        /* The run-time acquired page size */
static int  Page_size;

        /* SMP, Irix/Solaris mode, Linux 2.5.xx support */
static int   Cpu_tot,
            *Cpu_map;
        /* assume no IO-wait stats, overridden if linux 2.5.41 */
static const char *States_fmts = STATES_line2x4;

        /* Specific process id monitoring support */
static pid_t  Monpids [MONPIDMAX] = { 0 };
static int    Monpidsidx = 0;

        /* A postponed error message */
static char  Msg_delayed [SMLBUFSIZ];
static int   Msg_awaiting = 0;

        /* Configurable Display support ##################################*/

        /* Current screen dimensions.
           note: the number of processes displayed is tracked on a per window
                 basis (see the WIN_t).  Max_lines is the total number of
                 screen rows after deducting summary information overhead. */
        /* Current terminal screen size. */
static int  Screen_cols, Screen_rows, Max_lines;

        /* This is really the number of lines needed to display the summary
           information (0 - nn), but is used as the relative row where we
           stick the cursor between frames. */
static int  Msg_row;

        /* Global/Non-windows mode stuff that IS persistent (in rcfile) */
static int    Mode_altscr;      /* 'A' - 'Alt' display mode (multi windows)  */
        /* next toggle physically alters a proc_t, it CANNOT be window based */
static int    Mode_irixps = 1;  /* 'I' - Irix vs. Solaris mode (SMP-only)    */
static float  Delay_time = DEF_DELAY;  /* how long to sleep between updates  */

        /* Global/Non-windows mode stuff that is NOT persistent */
static int  No_ksyms = -1,      /* set to '0' if ksym avail, '1' otherwise   */
            PSDBopen = 0,       /* set to '1' if psdb opened (now postponed) */
            Batch = 0,          /* batch mode, collect no input, dumb output */
            Loops = -1,         /* number of iterations, -1 loops forever    */
            Secure_mode = 0;    /* set if some functionality restricted      */

        /* Some cap's stuff to reduce runtime calls --
           to accomodate 'Batch' mode, they begin life as empty strings */
static char  Cap_bold       [CAPBUFSIZ] = "",
             Cap_clr_eol    [CAPBUFSIZ] = "",
             Cap_clr_eos    [CAPBUFSIZ] = "",
             Cap_clr_scr    [CAPBUFSIZ] = "",
             Cap_curs_norm  [CAPBUFSIZ] = "",
             Cap_curs_huge  [CAPBUFSIZ] = "",
             Cap_home       [CAPBUFSIZ] = "",
             Cap_norm       [CAPBUFSIZ] = "",
             Cap_reverse    [CAPBUFSIZ] = "",
             Caps_off       [CAPBUFSIZ] = "";
static int   Cap_can_goto = 0;


        /* ////////////////////////////////////////////////////////////// */
        /* Special Section: multiple windows/field groups  ---------------*/

        /* The pointers to our four WIN_t's, and which of those is considered
           the 'current' window (ie. which window is associated with any summ
           info displayed and to which window commands are directed) */
static WIN_t *Winstk [GROUPSMAX],
             *Curwin;

        /* Frame oriented stuff that can't remain local to any 1 function
           and/or that would be too cumbersome managed as parms */
static int    Frame_maxtask;    /* last known number of active tasks */
                                /* ie. current 'size' of proc table  */
static float  Frame_tscale;     /* so we can '*' vs. '/' WHEN 'pcpu' */
static int    Frame_srtflg,     /* the subject window sort direction */
              Frame_ctimes,     /* the subject window's ctimes flag  */
              Frame_cmdlin;     /* the subject window's cmdlin flag  */
        /* ////////////////////////////////////////////////////////////// */


/*######  Sort callbacks  ################################################*/

        /*
         * These happen to be coded in the same order as the enum 'pflag'
         * values.  Note that 2 of these routines serve double duty --
         * 2 columns each.
         */
_SC_NUMx(P_PID, pid)
_SC_NUMx(P_PPD, ppid)
_SC_NUMx(P_PGD, pgrp)
_SC_NUMx(P_UID, euid)
_SC_STRx(P_USR, euser)
_SC_STRx(P_GRP, egroup)
_SC_NUMx(P_TTY, tty)
_SC_NUMx(P_PRI, priority)
_SC_NUMx(P_NCE, nice)
_SC_NUMx(P_CPN, processor)
_SC_NUM1(P_CPU, pcpu)
                                        /* also serves P_TM2 ! */
static int sort_P_TME (const proc_t **P, const proc_t **Q)
{
   if (Frame_ctimes) {
      if ( ((*P)->cutime + (*P)->cstime + (*P)->utime + (*P)->stime)
        < ((*Q)->cutime + (*Q)->cstime + (*Q)->utime + (*Q)->stime) )
           return SORT_lt;
      if ( ((*P)->cutime + (*P)->cstime + (*P)->utime + (*P)->stime)
        > ((*Q)->cutime + (*Q)->cstime + (*Q)->utime + (*Q)->stime) )
           return SORT_gt;
   } else {
      if ( ((*P)->utime + (*P)->stime) < ((*Q)->utime + (*Q)->stime))
         return SORT_lt;
      if ( ((*P)->utime + (*P)->stime) > ((*Q)->utime + (*Q)->stime))
         return SORT_gt;
   }
   return SORT_eq;
}

_SC_NUM1(P_VRT, size)
_SC_NUM2(P_SWP, size, resident)
_SC_NUM1(P_RES, resident)               /* also serves P_MEM ! */
_SC_NUM1(P_COD, trs)
_SC_NUM1(P_DAT, drs)
_SC_NUM1(P_SHR, share)
_SC_NUM1(P_FLT, maj_flt)
_SC_NUM1(P_DRT, dt)
_SC_NUMx(P_STA, state)

static int sort_P_CMD (const proc_t **P, const proc_t **Q)
{
   /* if a process doesn't have a cmdline, we'll consider it a kernel thread
      -- since show_a_task gives such tasks special treatment, we must too */
   if (Frame_cmdlin && ((*P)->cmdline || (*Q)->cmdline)) {
      if (!(*Q)->cmdline) return Frame_srtflg * -1;
      if (!(*P)->cmdline) return Frame_srtflg;
      return Frame_srtflg *
         strncmp((*Q)->cmdline[0], (*P)->cmdline[0], (unsigned)Curwin->maxcmdln);
   }
   /* this part also handles the compare if both are kernel threads */
   return Frame_srtflg * strcmp((*Q)->cmd, (*P)->cmd);
}

_SC_NUM1(P_WCH, wchan)
_SC_NUM1(P_FLG, flags)


/*######  Tiny useful routine(s)  ########################################*/

        /*
         * This routine isolates ALL user INPUT and ensures that we
         * wont be mixing I/O from stdio and low-level read() requests */
static int chin (int ech, char *buf, unsigned cnt)
{
   int rc;

   fflush(stdout);
   if (!ech)
      rc = read(STDIN_FILENO, buf, cnt);
   else {
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &Savedtty);
      rc = read(STDIN_FILENO, buf, cnt);
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &Rawtty);
   }
      /* may be the beginning of a lengthy escape sequence  */
   tcflush(STDIN_FILENO, TCIFLUSH);
   return rc;                   /* note: we do NOT produce a vaid 'string' */
}


        /*
         * This routine simply formats whatever the caller wants and
         * returns a pointer to the resulting 'const char' string... */
static const char *fmtmk (const char *fmts, ...)
{
   static char buf[BIGBUFSIZ];          /* with help stuff, our buffer */
   va_list va;                          /* requirements exceed 1k */

   va_start(va, fmts);
   vsnprintf(buf, sizeof(buf), fmts, va);
   va_end(va);
   return (const char *)buf;
}


        /*
         * This guy was originally designed just to trim the rc file lines and
         * any 'open_psdb_message' result which arrived with an inappropriate
         * newline (thanks to 'sysmap_mmap') -- but when tabs (^I) were found
         * in some proc cmdlines, a choice was offered twix space or null. */
static char *strim (int sp, char *str)
{
   static const char ws[] = "\b\f\n\r\t\v";
   char *p;

   if (sp)
      while ((p = strpbrk(str, ws))) *p = ' ';
   else
      if ((p = strpbrk(str, ws))) *p = 0;
   return str;
}


        /*
         * This guy just facilitates Batch and protects against dumb ttys
         * -- we'd 'inline' him but he's only called twice per frame,
         * yet used in many other locations. */
static const char *tg2 (int x, int y)
{
   return Cap_can_goto ? tgoto(cursor_address, x, y) : "";
}


/*######  Exit/Interrput routines  #######################################*/

        /*
         * The usual program end --
         * called only by functions in this section. */
static void bye_bye (int eno, const char *str)
{
   if (!Batch)
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &Savedtty);
   putp(tg2(0, Screen_rows));
   putp(Cap_curs_norm);
   putp("\n");

#ifdef ATEOJ_REPORT
   fprintf(stderr,
      "\nbye_bye's Summary report:"
      "\n\tProgram"
      "\n\t   Linux version = %u.%u.%u"
      "\n\t   Page_size = %d, Cpu_tot = %d"
      "\n\t   %s, using Hertz = %u (%u bytes, %u-bit time)"
      "\n\t   sizeof(CPUS_t) = %u, sizeof(HIST_t) = %u (%u HIST_t's/Page)"
      "\n\t   CPU_FMTS_JUST1 = %s"
      "\n\t   CPU_FMTS_MULTI = %s"
      "\n\tTerminal: %s"
      "\n\t   device = %s, ncurses = v%s"
      "\n\t   max_colors = %d, max_pairs = %d"
      "\n\t   Cap_can_goto = %s"
      "\n\t   Screen_cols = %d, Screen_rows = %d"
      "\n\t   Max_lines = %d"
      "\n\tWindows and Curwin->"
      "\n\t   sizeof(WIN_t) = %u, GROUPSMAX = %d"
      "\n\t   winname = %s, grpname = %s"
#ifdef CASEUP_HEXES
      "\n\t   winflags = %08X, maxpflgs = %d"
#else
      "\n\t   winflags = %08x, maxpflgs = %d"
#endif
      "\n\t   fieldscur = %s"
      "\n\t   winlines  = %d, maxtasks = %d, maxcmdln = %d"
      "\n\t   sortindx  = %d"
      "\n"
      , LINUX_VERSION_MAJOR(linux_version_code)
      , LINUX_VERSION_MINOR(linux_version_code)
      , LINUX_VERSION_PATCH(linux_version_code)
      , Page_size, Cpu_tot
      , procps_version, (unsigned)Hertz, sizeof(Hertz), sizeof(Hertz) * 8
      , sizeof(CPUS_t), sizeof(HIST_t), Page_size / sizeof(HIST_t)
      , CPU_FMTS_JUST1, CPU_FMTS_MULTI
#ifdef PRETENDNOCAP
      , "dumb"
#else
      , termname()
#endif
      , ttyname(STDOUT_FILENO), NCURSES_VERSION
      , max_colors, max_pairs
      , Cap_can_goto ? "yes" : "No!"
      , Screen_cols, Screen_rows
      , Max_lines
      , sizeof(WIN_t), GROUPSMAX
      , Curwin->winname, Curwin->grpname
      , Curwin->winflags, Curwin->maxpflgs
      , Curwin->fieldscur
      , Curwin->winlines, Curwin->maxtasks, Curwin->maxcmdln
      , Curwin->sortindx
      );
#endif

   if (str) {
      if (eno) perror(str);
      else {
         fputs(str, stderr);
         eno = 1;
      }
   }
   exit(eno);
}


        /*
         * Normal end of execution.
         * catches:
         *    SIGALRM, SIGHUP, SIGINT, SIGPIPE, SIGQUIT and SIGTERM */
static void stop (int dont_care_sig)
{
   (void)dont_care_sig;
   bye_bye(0, NULL);
}


        /*
         * Standard error handler to normalize the look of all err o/p */
static void std_err (const char *str)
{
   static char buf[SMLBUFSIZ];

   fflush(stdout);
   /* we'll use our own buffer so callers can still use fmtmk() and, yes the
      leading tab is not the standard convention, but the standard is wrong
      -- OUR msg won't get lost in screen clutter, like so many others! */
   snprintf(buf, sizeof(buf), "\t%s: %s\n", Myname, str);
   if (!Ttychanged) {
      fprintf(stderr, buf);
      exit(1);
   }
      /* not to worry, he'll change our exit code to 1 due to 'buf' */
   bye_bye(0, buf);
}


        /*
         * Suspend ourself.
         * catches:
         *    SIGTSTP, SIGTTIN and SIGTTOU */
static void suspend (int dont_care_sig)
{
  (void)dont_care_sig;
      /* reset terminal */
   tcsetattr(STDIN_FILENO, TCSAFLUSH, &Savedtty);
   putp(tg2(0, Screen_rows));
   putp(Cap_curs_norm);
   fflush(stdout);
   raise(SIGSTOP);
      /* later, after SIGCONT... */
   if (!Batch)
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &Rawtty);
}


/*######  Misc Color/Display support  ####################################*/

        /*
         * Make the appropriate caps/color strings and set some
         * lengths which are used to distinguish twix the displayed
         * columns and an actual printed row!
         * note: we avoid the use of background color so as to maximize
         *       compatibility with the user's xterm settings */
static void capsmk (WIN_t *q)
{
   /* macro to test if a basic (non-color) capability is valid
         thanks: Floyd Davidson <floyd@ptialaska.net> */
#define tIF(s)  s ? s : ""
   static int capsdone = 0;

      /* we must NOT disturb our 'empty' terminfo strings! */
   if (Batch) return;

      /* these are the unchangeable puppies, so we only do 'em once */
   if (!capsdone) {
      strcpy(Cap_bold, tIF(enter_bold_mode));
      strcpy(Cap_clr_eol, tIF(clr_eol));
      strcpy(Cap_clr_eos, tIF(clr_eos));
      strcpy(Cap_clr_scr, tIF(clear_screen));
      strcpy(Cap_curs_huge, tIF(cursor_visible));
      strcpy(Cap_curs_norm, tIF(cursor_normal));
      strcpy(Cap_home, tIF(cursor_home));
      strcpy(Cap_norm, tIF(exit_attribute_mode));
      strcpy(Cap_reverse, tIF(enter_reverse_mode));
      snprintf(Caps_off, sizeof(Caps_off), "%s%s", Cap_norm, tIF(orig_pair));
      if (tgoto(cursor_address, 1, 1)) Cap_can_goto = 1;
      capsdone = 1;
   }
      /* the key to NO run-time costs for configurable colors -- we spend a
         little time with the user now setting up our terminfo strings, and
         the job's done until he/she/it has a change-of-heart */
   if (CHKw(q, Show_COLORS) && max_colors > 0) {
      strcpy(q->capclr_sum, tparm(set_a_foreground, q->summclr));
      snprintf(q->capclr_msg, sizeof(q->capclr_msg), "%s%s"
         , tparm(set_a_foreground, q->msgsclr), Cap_reverse);
      snprintf(q->capclr_pmt, sizeof(q->capclr_pmt), "%s%s"
         , tparm(set_a_foreground, q->msgsclr), Cap_bold);
      snprintf(q->capclr_hdr, sizeof(q->capclr_hdr), "%s%s"
         , tparm(set_a_foreground, q->headclr), Cap_reverse);
      snprintf(q->capclr_rownorm, sizeof(q->capclr_rownorm), "%s%s"
         , Caps_off, tparm(set_a_foreground, q->taskclr));
   } else {
      q->capclr_sum[0] = '\0';
      strcpy(q->capclr_msg, Cap_reverse);
      strcpy(q->capclr_pmt, Cap_bold);
      strcpy(q->capclr_hdr, Cap_reverse);
      strcpy(q->capclr_rownorm, Cap_norm);
   }
      /* this guy's a composite, so we do him outside the if */
   snprintf(q->capclr_rowhigh, sizeof(q->capclr_rowhigh), "%s%s"
      , q->capclr_rownorm, CHKw(q, Show_HIBOLD) ? Cap_bold : Cap_reverse);
   q->len_rownorm = strlen(q->capclr_rownorm);
   q->len_rowhigh = strlen(q->capclr_rowhigh);

#undef tIF
}


        /*
         * Show an error, but not right now.
         * Due to the postponed opening of ksym, using open_psdb_message,
         * if P_WCH had been selected and the program is restarted, the
         * message would otherwise be displayed prematurely.
         * (old top handles that situation with typical inelegance) */
static void msg_save (const char *fmts, ...)
{
   char tmp[SMLBUFSIZ];
   va_list va;

   va_start(va, fmts);
   vsnprintf(tmp, sizeof(tmp), fmts, va);
   va_end(va);
      /* we'll add some extra attention grabbers to whatever this is */
   snprintf(Msg_delayed, sizeof(Msg_delayed), "\a***  %s  ***", strim(0, tmp));
   Msg_awaiting = 1;
}


        /*
         * Show an error message (caller may include a '\a' for sound) */
static void show_msg (const char *str)
{
   PUTP("%s%s %s %s%s"
      , tg2(0, Msg_row)
      , Curwin->capclr_msg
      , str
      , Caps_off
      , Cap_clr_eol);
   fflush(stdout);
   sleep(MSG_SLEEP);
   Msg_awaiting = 0;
}


        /*
         * Show an input prompt + larger cursor */
static void show_pmt (const char *str)
{
   PUTP("%s%s%s: %s%s"
      , tg2(0, Msg_row)
      , Curwin->capclr_pmt
      , str
      , Cap_curs_huge
      , Caps_off);
   fflush(stdout);
}


        /*
         * Show lines with specially formatted elements, but only output
         * what will fit within the current screen width.
         *    Our special formatting consists of:
         *       "some text <_delimiter_> some more text <_delimiter_>...\n"
         *    Where <_delimiter_> is a single byte in the range of:
         *       \01 through \10  (in decimalizee, 1 - 8)
         *    and is used to select an 'attribute' from a capabilities table
         *    which is then applied to the *preceding* substring.
         * Once recognized, the delimiter is replaced with a null character
         * and viola, we've got a substring ready to output!  Strings or
         * substrings without delimiters will receive the Cap_norm attribute.
         *
         * Caution:
         *    This routine treats all non-delimiter bytes as displayable
         *    data subject to our screen width marching orders.  If callers
         *    embed non-display data like tabs or terminfo strings in our
         *    glob, a line will truncate incorrectly at best.  Worse case
         *    would be truncation of an embedded tty escape sequence.
         *
         *    Tabs must always be avoided or our efforts are wasted and
         *    lines will wrap.  To lessen but not eliminate the risk of
         *    terminfo string truncation, such non-display stuff should
         *    be placed at the beginning of a "short" line.
         *    (and as for tabs, gimme 1 more color then no worries, mate) */
static void show_special (const char *glob)
{ /* note: the following is for documentation only,
           the real captab is now found in a group's WIN_t !
     +------------------------------------------------------+
     | char *captab[] = {                 :   Cap's/Delim's |
     |   Cap_norm, Cap_norm, Cap_bold,    =   \00, \01, \02 |
     |   Sum_color,                       =   \03           |
     |   Msg_color, Pmt_color,            =   \04, \05      |
     |   Hdr_color,                       =   \06           |
     |   Row_color_high,                  =   \07           |
     |   Row_color_norm  };               =   \10 [octal!]  |
     +------------------------------------------------------+ */
   char tmp[BIGBUFSIZ], *cap, *lin_end, *sub_beg, *sub_end;
   int room;

      /* handle multiple lines passed in a bunch */
   while ((lin_end = strchr(glob, '\n'))) {

         /* create a local copy we can extend and otherwise abuse */
      memcpy(tmp, glob, (unsigned)(lin_end - glob));
         /* zero terminate this part and prepare to parse substrings */
      tmp[lin_end - glob] = '\0';
      room = Screen_cols;
      sub_beg = sub_end = tmp;

      while (*sub_beg) {
         switch (*sub_end) {
            case 0:                     /* no end delim, captab makes normal */
               *(sub_end + 1) = '\0';   /* extend str end, then fall through */
            case 1: case 2: case 3: case 4:
            case 5: case 6: case 7: case 8:
               cap = Curwin->captab[(int)*sub_end];
               *sub_end = '\0';
               PUTP("%s%.*s%s", cap, room, sub_beg, Caps_off);
               room -= (sub_end - sub_beg);
               sub_beg = ++sub_end;
               break;
            default:                    /* nothin' special, just text */
               ++sub_end;
         }
         if (0 >= room) break;          /* skip substrings that won't fit */
      } /* end: while 'subtrings' */

      putp(Cap_clr_eol);
      putp("\n");                       /* emulate truncated newline */
      glob = ++lin_end;                 /* point to next line (maybe) */
   } /* end: while 'lines' */

   /* if there's anything left in the glob (by virtue of no trailing '\n'),
      it probably means caller wants to retain cursor position on this final
      line -- ok then, we'll just do our 'fit-to-screen' thingy... */
   if (*glob) PUTP("%.*s", Screen_cols, glob);
   fflush(stdout);
}


/*######  Small Utility routines  ########################################*/

        /*
         * Get a string from the user */
static char *ask4str (const char *prompt)
{
   static char buf[GETBUFSIZ];

   show_pmt(prompt);
   memset(buf, '\0', sizeof(buf));
   chin(1, buf, sizeof(buf) - 1);
   putp(Cap_curs_norm);
   return strim(0, buf);
}


        /*
         * Get a float from the user */
static float get_float (const char *prompt)
{
   char *line;
   float f;

   if (!(*(line = ask4str(prompt)))) return -1;
      /* note: we're not allowing negative floats */
   if (strcspn(line, ",.1234567890")) {
      show_msg("\aNot valid");
      return -1;
   }
   sscanf(line, "%f", &f);
   return f;
}


        /*
         * Get an integer from the user */
static int get_int (const char *prompt)
{
   char *line;
   int n;

   if (!(*(line = ask4str(prompt)))) return -1;
      /* note: we've got to allow negative ints (renice)  */
   if (strcspn(line, "-1234567890")) {
      show_msg("\aNot valid");
      return -1;
   }
   sscanf(line, "%d", &n);
   return n;
}


        /*
         * Do some scaling stuff.
         * We'll interpret 'num' as one of the following types and
         * try to format it to fit 'width'.
         *    SK_no (0) it's a byte count
         *    SK_Kb (1) it's kilobytes
         *    SK_Mb (2) it's megabytes
         *    SK_Gb (3) it's gigabytes  */
static const char *scale_num (unsigned num, const int width, const unsigned type)
{
      /* kilobytes, megabytes, gigabytes, duh! */
   static float scale[] = { 1024, 1024*1024, 1024*1024*1024, 0 };
      /* kilo, mega, giga, none */
#ifdef CASEUP_SCALE
   static char nextup[] =  { 'K', 'M', 'G', 0 };
#else
   static char nextup[] =  { 'k', 'm', 'g', 0 };
#endif
   static char buf[TNYBUFSIZ];
   float *dp;
   char *up;

      /* try an unscaled version first... */
   if (width >= snprintf(buf, sizeof(buf), "%u", num)) return buf;

      /* now try successively higher types until it fits */
   for (up = nextup + type, dp = scale; *dp; ++dp, ++up) {
         /* the most accurate version */
      if (width >= snprintf(buf, sizeof(buf), "%.1f%c", num / *dp, *up))
         return buf;
         /* the integer version */
      if (width >= snprintf(buf, sizeof(buf), "%d%c", (int)(num / *dp), *up))
         return buf;
   }
      /* well shoot, this outta' fit... */
   return "?";
}


        /*
         * Do some scaling stuff.
         * Format 'tics' to fit 'width' */
static const char *scale_tics (TICS_t tics, const int width)
{
#define T1 "%u:%02u.%02u"
#define T2 "%u:%02u"
#ifdef CASEUP_SCALE
#define HH "%uH"
#define DD "%uD"
#define WW "%uW"
#else
#define HH "%uh"
#define DD "%ud"
#define WW "%uw"
#endif
   static char buf[TNYBUFSIZ];
   unsigned ss;
   unsigned nt; // narrow time, for speed on 32-bit
   unsigned ct; // centiseconds past the second

   ct  = ((tics * 100) / Hertz)%100 ;
   nt  = tics / Hertz;
   if (width >= snprintf(buf, sizeof(buf), T1, nt/60, nt%60, ct))
         return buf;
   ss  = nt % 60;
   nt  /= 60;
   if (width >= snprintf(buf, sizeof buf, T2, nt, ss))
      return buf;
   nt  /= 60;
   if (width >= snprintf(buf, sizeof buf, HH, nt))
      return buf;
   nt  /= 24;
   if (width >= snprintf(buf, sizeof buf, DD, nt))
      return buf;
   nt /= 7;
   if (width >= snprintf(buf, sizeof buf, WW, nt))
      return buf;

      /* well shoot, this outta' fit... */
   return "?";

#undef T1
#undef T2
#undef HH
#undef DD
#undef WW
}


        /*
         * Calculate and the elapsed time since the last update along with the
         * scaling factor used in multiplication (vs. division) when calculating
         * a displayable task's %CPU. */
static void time_elapsed (void)
{
    static struct timeval oldtimev;
    struct timeval timev;
    struct timezone timez;
    float et;

    gettimeofday(&timev, &timez);
    et = (timev.tv_sec - oldtimev.tv_sec)
       + (float)(timev.tv_usec - oldtimev.tv_usec) / 1000000.0;
    oldtimev.tv_sec = timev.tv_sec;
    oldtimev.tv_usec = timev.tv_usec;
      /* if in Solaris mode, adjust our scaling for all cpus */
    Frame_tscale = 100.0f / ((float)Hertz * (float)et * (Mode_irixps ? 1 : Cpu_tot));
}


/*######  Library Alternatives  ##########################################*/

        /*
         * Handle our own memory stuff without the risk of leaving the
         * user's terminal in an ugly state should things go sour. */

static void *alloc_c (unsigned numb)
{
   void * p;

   if (!numb) ++numb;
   if (!(p = calloc(1, numb)))
      std_err("failed memory allocate");
   return p;
}


static void *alloc_r (void *q, unsigned numb)
{
   void *p;

   if (!numb) ++numb;
   if (!(p = realloc(q, numb)))
      std_err("failed memory allocate");
   return p;
}


        /*
         * This guy's modeled on libproc's 'five_cpu_numbers' function except
         * we preserve all cpu data in our CPUS_t array which is organized
         * as follows:
         *    cpus[0] thru cpus[n] == tics for each separate cpu
         *    cpus[Cpu_tot]        == tics from the 1st /proc/stat line */
static CPUS_t *refreshcpus (CPUS_t *cpus)
{
   static FILE *fp = NULL;
   int i;
      /* enough for a /proc/stat CPU line (not the intr line) */
   char buf[SMLBUFSIZ];

      /* by opening this file once, we'll avoid the hit on minor page faults
         (sorry Linux, but you'll have to close it for us) */
   if (!fp) {
      if (!(fp = fopen("/proc/stat", "r")))
         std_err(fmtmk("Failed /proc/stat open: %s", strerror(errno)));
      /* note: we allocate one more CPUS_t than Cpu_tot so that the last slot
               can hold tics representing the /proc/stat cpu summary (the first
               line read) -- that slot supports our View_CPUSUM toggle */
      cpus = alloc_c((1 + Cpu_tot) * sizeof(CPUS_t));
   }
   rewind(fp);
   fflush(fp);

      /* first value the last slot with the cpu summary line */
   if (!fgets(buf, sizeof(buf), fp)) std_err("failed /proc/stat read");
   if (4 > sscanf(buf, CPU_FMTS_JUST1
      , &cpus[Cpu_tot].u, &cpus[Cpu_tot].n, &cpus[Cpu_tot].s, &cpus[Cpu_tot].i, &cpus[Cpu_tot].w))
         std_err("failed /proc/stat read");
      /* and just in case we're 2.2.xx compiled without SMP support... */
   if (1 == Cpu_tot) memcpy(cpus, &cpus[1], sizeof(CPUS_t));

      /* and now value each separate cpu's tics */
   for (i = 0; 1 < Cpu_tot && i < Cpu_tot; i++) {
#ifdef PRETEND4CPUS
      rewind(fp);
#endif
      if (!fgets(buf, sizeof(buf), fp)) std_err("failed /proc/stat read");
      if (4 > sscanf(buf, CPU_FMTS_MULTI
         , &cpus[i].u, &cpus[i].n, &cpus[i].s, &cpus[i].i, &cpus[i].w))
            std_err("failed /proc/stat read");
   }

   return cpus;
}


        /*
         * This guy's modeled on libproc's 'readproctab' function except
         * we reuse and extend any prior proc_t's.  He's been customized
         * for our specific needs and to avoid the use of <stdarg.h> */
static proc_t **refreshprocs (proc_t **table, int flags)
{
#define PTRsz  sizeof(proc_t *)         /* eyeball candy */
#define ENTsz  sizeof(proc_t)
   static unsigned savmax = 0;          /* first time, Bypass: (i)  */
   proc_t *ptsk = (proc_t *)-1;         /* first time, Force: (ii)  */
   unsigned curmax = 0;                 /* every time  (jeeze)      */
   PROCTAB* PT;

   if (Monpidsidx) {
      PT = openproc(flags | PROC_PID, Monpids);
         /* work around a previous bug in openproc (now corrected) */
      PT->procfs = NULL;
   } else
      PT = openproc(flags);

      /* i) Allocated Chunks:  *Existing* table;  refresh + reuse */
   while (curmax < savmax) {
      if (table[curmax]->cmdline) {
         free(*table[curmax]->cmdline);
         table[curmax]->cmdline = NULL;
      }
      if (!(ptsk = readproc(PT, table[curmax]))) break;
      ++curmax;
   }

      /* ii) Unallocated Chunks:  *New* or *Existing* table;  extend + fill */
   while (ptsk) {
         /* realloc as we go, keeping 'table' ahead of 'currmax++' */
      table = alloc_r(table, (curmax + 1) * PTRsz);
         /* here, readproc will allocate the underlying proc_t stg */
      if ((ptsk = readproc(PT, NULL)))
         table[curmax++] = ptsk;
   }
   closeproc(PT);

      /* iii) Chunkless:  make 'eot' entry, after possible extension */
   if (curmax >= savmax) {
      table = alloc_r(table, (curmax + 1) * PTRsz);
         /* here, we must allocate the underlying proc_t stg ourselves */
      table[curmax] = alloc_c(ENTsz);
      savmax = curmax + 1;
   }
      /* this frame's end, but not necessarily end of allocated space */
   table[curmax]->pid = -1;
   return table;

#undef PTRsz
#undef ENTsz
}


/*######  Startup routines  ##############################################*/

        /*
         * No mater what *they* say, we handle the really really BIG and
         * IMPORTANT stuff upon which all those lessor functions depend! */
static void before (char *me)
{
   int i;

      /* setup our program name -- big! */
   Myname = strrchr(me, '/');
   if (Myname) ++Myname; else Myname = me;

      /* establish cpu particulars -- even bigger! */
#ifdef PRETEND4CPUS
   Cpu_tot = 4;
#else
   Cpu_tot = smp_num_cpus;
#endif
   Cpu_map = alloc_r(NULL, sizeof(int) * Cpu_tot);
   for (i = 0; i < Cpu_tot; i++)
      Cpu_map[i] = i;
   if(linux_version_code > LINUX_VERSION(2, 5, 41))
      States_fmts = STATES_line2x5;

      /* get virtual page size -- nearing huge! */
   Page_size = getpagesize();
}


        /*
         * Build the local RC file name then try to read both of 'em.
         * 'SYS_RCFILE' contains two lines consisting of the secure
         *   mode switch and an update interval.  It's presence limits what
         *   ordinary users are allowed to do.
         * '$HOME/RCfile' contains multiple lines - 2 global + 3 per window.
         *   line 1: a shameless advertisement
         *   line 2: an id, Mode_altcsr, Mode_irixps, Delay_time and Curwin.
         *           If running in secure mode via the /etc/rcfile,
         *           Delay_time will be ignored except for root.
         * For each of the 4 windows:
         *   line a: contains w->winname, fieldscur
         *   line b: contains w->winflags, sortindx, maxtasks
         *   line c: contains w->summclr, msgsclr, headclr, taskclr */
static void configs_read (void)
{
   static const char err_rc[] = "bad rcfile, you should delete '%s'";
   char fbuf[SMLBUFSIZ];
   FILE *fp;
   float delay = DEF_DELAY;
   char id;
   int i;

   snprintf(RCfile, sizeof(RCfile), ".%src", Myname);
   if (getenv("HOME"))
      snprintf(RCfile, sizeof(RCfile), "%s/.%src", getenv("HOME"), Myname);

   fp = fopen(SYS_RCFILE, "r");
   if (fp) {
      fbuf[0] = '\0';
      fgets(fbuf, sizeof(fbuf), fp);            /* sys rc file, line #1 */
      if (strchr(fbuf, 's')) Secure_mode = 1;

      fbuf[0] = '\0';
      fgets(fbuf, sizeof(fbuf), fp);            /* sys rc file, line #2 */
      fclose(fp);
      sscanf(fbuf, "%f", &delay);
   }
   fp = fopen(RCfile, "r");
   if (fp) {
      fgets(fbuf, sizeof(fbuf), fp);    /* ignore shameless advertisement */
      if (5 != (fscanf(fp, "Id:%c, "
         "Mode_altscr=%d, Mode_irixps=%d, Delay_time=%f, Curwin=%d\n"
         , &id, &Mode_altscr, &Mode_irixps, &delay, &i)) || RCF_FILEID != id)
            std_err(fmtmk(err_rc, RCfile));

         /* you saw that, right?  (fscanf stickin' it to 'i') */
      Curwin = Winstk[i];
      for (i = 0; i < GROUPSMAX; i++) {
           /* we won't check fscanf returns from here on out -- we'll be
              hunky-dory with nothing in an rcfile except the 1st 2 lines */
         fscanf(fp, "%s\tfieldscur=%s\n"
            , Winstk[i]->winname, Winstk[i]->fieldscur);
         if (WINNAMSIZ <= strlen(Winstk[i]->winname)
         || strlen(DEF_FIELDS) != strlen(Winstk[i]->fieldscur))
            std_err(fmtmk(err_rc, RCfile));
         fscanf(fp, "\twinflags=%d, sortindx=%u, maxtasks=%d \n"
            , &Winstk[i]->winflags
            , &Winstk[i]->sortindx
            , &Winstk[i]->maxtasks);
         fscanf(fp, "\tsummclr=%d, msgsclr=%d, headclr=%d, taskclr=%d \n"
            , &Winstk[i]->summclr
            , &Winstk[i]->msgsclr
            , &Winstk[i]->headclr
            , &Winstk[i]->taskclr);
      }
      fclose(fp);
   }
      /* lastly, establish the true runtime secure mode and delay time */
   Secure_mode = getuid() ? Secure_mode : 0;
   if (!Secure_mode || !getuid()) Delay_time = delay;
}


        /*
         * Parse command line arguments.
         * Note: it's assumed that the rc file(s) have already been read
         *       and our job is to see if any of those options are to be
         *       overridden -- we'll force some on and negate others in our
         *       best effort to honor the loser's (oops, user's) wishes... */
static void parse_args (char **args)
{
   /* differences between us and the former top:
      -C (separate CPU states for SMP) is left to an rcfile
      -p (pid monitoring) allows, not requires, a comma delimited list
      -q (zero delay) eliminated as redundant, incomplete and inappropriate
            use: "nice -n-10 top -d0" to achieve what was only claimed
      -c,i,S act as toggles (not 'on' switches) for enhanced user flexibility
      .  no deprecated/illegal use of 'breakargv:' with goto
      .  bunched args are actually handled properly and none are ignored
      .  we tolerate NO whitespace and NO switches -- maybe too tolerant? */
   static const char usage[] =
      " -h?v | -bcisS -d delay -n iterations -p pid [,pid ...]";
   float tmp_delay = MAXFLOAT;
   char *p;

   while (*args) {
      char *cp = *(args++);

      while (*cp) {
         switch (*cp) {
            case '\0':
            case '-':
               break;
            case 'b':
               Batch = 1;
               break;
            case 'c':
               TOGw(Curwin, Show_CMDLIN);
               break;
            case 'd':
               if (cp[1]) ++cp;
               else if (*args) cp = *args++;
               else std_err("-d requires argument");
                  /* a negative delay will be dealt with shortly... */
               if (1 != sscanf(cp, "%f", &tmp_delay))
                  std_err(fmtmk("bad delay '%s'", cp));
               break;
            case '?':
            case 'h': case 'H':
            case 'v': case 'V':
               std_err(fmtmk("%s\nusage:\t%s%s"
                  , procps_version, Myname, usage));
            case 'i':
               TOGw(Curwin, Show_IDLEPS);
               Curwin->maxtasks = 0;
               break;
            case 'n':
               if (cp[1]) cp++;
               else if (*args) cp = *args++;
               else std_err("-n requires argument");
               if (1 != sscanf(cp, "%d", &Loops) || 1 > Loops)
                  std_err(fmtmk("bad iterations arg '%s'", cp));
               break;
            case 'p':
               do {
                  if (cp[1]) cp++;
                  else if (*args) cp = *args++;
                  else std_err("-p argument missing");
                  if (Monpidsidx >= MONPIDMAX)
                     std_err(fmtmk("pid limit (%d) exceeded", MONPIDMAX));
                  if (1 != sscanf(cp, "%d", &Monpids[Monpidsidx])
                  || 0 > Monpids[Monpidsidx])
                     std_err(fmtmk("bad pid '%s'", cp));
                  if (!Monpids[Monpidsidx])
                     Monpids[Monpidsidx] = getpid();
                  Monpidsidx++;
                  if (!(p = strchr(cp, ',')))
                     break;
                  cp = p;
               } while (*cp);
               break;
            case 's':
               Secure_mode = 1;
               break;
            case 'S':
               TOGw(Curwin, Show_CTIMES);
               break;
            default :
               std_err(fmtmk("unknown argument '%c'\nusage:\t%s%s"
                  , *cp, Myname, usage));

         } /* end: switch (*cp) */

            /* advance cp and jump over any numerical args used above */
         if (*cp) cp += strspn(&cp[1], "- ,.1234567890") + 1;
      } /* end: while (*cp) */
   } /* end: while (*args) */

      /* fixup delay time, maybe... */
   if (MAXFLOAT != tmp_delay) {
      if (Secure_mode || 0 > tmp_delay)
         msg_save("Delay time Not changed");
      else
         Delay_time = tmp_delay;
   }
}


        /*
         * Process command line arguments.
         * Note: it's assumed that the rc file(s) have already been read
         *       and our job is to see if any of those options are to be
         *       overridden  */
int
process_command_line_arguments(int *argc, char **argv)
{
	char **av = argv;
	int count = *argc;
	float tmp_delay = MAXFLOAT;
	char *p;
	static const char usage[] =
      " -h?v | -bcisS -d delay -n iterations -p pid [,pid ...]";

	(*argc)--, av++;
	while((*argc > 0) && ('-' == *av[0])) {
		// for case of command option like '--xxx'
		// 'xxx' treat as a option in program 
		if('-' == *(av[0]+1)) {
			p = av[0];
			if((!strcmp(p + 2, "help")) || (!strcmp(p + 2, "version"))) {
			   std_err(fmtmk("%s\nusage:\t%s%s"
				  , procps_version, Myname, usage));
			} //else if(!strcmp(p + 2, "version")) {
			//}
			 else {
               std_err(fmtmk("unknown argument '%s'\nusage:\t%s%s"
                  , p + 2, Myname, usage));
			} 
		}
		// for case of '-a' or '-ax', 
		// every letter treat as a option 
		while(*++av[0]) switch(*av[0]) {
			case 'b':
				Batch = 1;
				break;
			case 'c':
				TOGw(Curwin, Show_CMDLIN);
				break;
			case 'd':
				if(*(av[0]+1)) av[0]++;
				else if(av[1]) { 
					av++; (*argc)--;
				} else
					std_err("-d requires argument");
				if(1 != sscanf(av[0], "%f", &tmp_delay))
					std_err(fmtmk("bad delay '%s'", av[0]));
				break;
			case '?':
			case 'h': case 'H':
			case 'v': case 'V':
			   std_err(fmtmk("%s\nusage:\t%s%s"
				  , procps_version, Myname, usage));
			case 'i':
				TOGw(Curwin, Show_IDLEPS);
				Curwin->maxtasks = 0;
				break;
			case 'n':
				if (*(av[0]+1)) av[0]++;
				else if (av[1]) {
					av++; (*argc)--;
				} else std_err("-n requires argument");
				if(1 != sscanf(av[0], "%d", &Loops) || 1 > Loops)
					std_err(fmtmk("bad iteration arg '%s'", av[0]));
				break;
			case 'p':
				do {
					if (*(av[0]+1)) av[0]++;
					else if (av[1]) {
						av++; (*argc)--;
					} else std_err("-p argument missing");
					if(Monpidsidx >= MONPIDMAX)
						std_err(fmtmk("pid limit (%d) exceeded", MONPIDMAX));
					if (1 != sscanf(av[0], "%d", &Monpids[Monpidsidx])
					|| 0 > Monpids[Monpidsidx])
						std_err(fmtmk("bad pid '%s'", av[0]));
					if(!Monpids[Monpidsidx])
						Monpids[Monpidsidx] = getpid();
					Monpidsidx++;
					if(!(p = strchr(av[0], ',')))
						break;
					av[0] = p;
				} while (*av[0]);
				break;
			case 's':
				Secure_mode = 1;
				break;
			case 'S':
				TOGw(Curwin, Show_CTIMES);
				break;
			default:
               std_err(fmtmk("unknown argument '%c'\nusage:\t%s%s"
                  , *av[0], Myname, usage));
		}
		(*argc)--, av++;
	}

	if(MAXFLOAT != tmp_delay) {
		if(Secure_mode || 0 > tmp_delay)
			msg_save("Delay time Not changed");
		else
			Delay_time = tmp_delay;
	}

	return (count - *argc);
}


        /*
         * Set up the terminal attributes */
static void whack_terminal (void)
{
   struct termios newtty;

      /* first the curses part... */
#ifdef PRETENDNOCAP
   setupterm("dumb", STDOUT_FILENO, NULL);
#else
   setupterm(NULL, STDOUT_FILENO, NULL);
#endif
      /* now our part... */
   if (!Batch) {
      if (-1 == tcgetattr(STDIN_FILENO, &Savedtty))
         std_err("failed tty get");
      newtty = Savedtty;
      newtty.c_lflag &= ~ICANON;
      newtty.c_lflag &= ~ECHO;
      newtty.c_cc[VMIN] = 1;
      newtty.c_cc[VTIME] = 0;

      Ttychanged = 1;
      if (-1 == tcsetattr(STDIN_FILENO, TCSAFLUSH, &newtty)) {
         putp(Cap_clr_scr);
         std_err(fmtmk("failed tty set: %s", strerror(errno)));
      }
      tcgetattr(STDIN_FILENO, &Rawtty);
      putp(Cap_clr_scr);
      fflush(stdout);
   }
}


/*######  Field Selection/Ordering routines  #############################*/

        /* These are our gosh darn 'Fields' !
           They MUST be kept in sync with pflags !! */
static FTAB_t  Fieldstab[] = {
/*   head           fmts     width   scale  sort      desc
     -----------    -------  ------  -----  --------  ---------------------- */
   { "  PID ",      "%5d ",     -1,    -1, _SF(P_PID), "Process Id"           },
   { " PPID ",      "%5d ",     -1,    -1, _SF(P_PPD), "Parent Process Pid"   },
   { " PGID ",      "%5d ",     -1,    -1, _SF(P_PGD), "Process Group Id"     },
   { " UID ",       "%4d ",     -1,    -1, _SF(P_UID), "User Id"              },
   { "USER     ",   "%-8.8s ",  -1,    -1, _SF(P_USR), "User Name"            },
   { "GROUP    ",   "%-8.8s ",  -1,    -1, _SF(P_GRP), "Group Name"           },
   { "TTY      ",   "%-8.8s ",   8,    -1, _SF(P_TTY), "Controlling Tty"      },
   { " PR ",        "%3ld ",    -1,    -1, _SF(P_PRI), "Priority"             },
   { " NI ",        "%3ld ",    -1,    -1, _SF(P_NCE), "Nice value"           },
   { "#C ",         "%2d ",     -1,    -1, _SF(P_CPN), "Last used cpu (SMP)"  },
   { "%CPU ",       "%#4.1f ",  -1,    -1, _SF(P_CPU), "CPU usage"            },
   { "  TIME ",     "%6.6s ",    6,    -1, _SF(P_TME), "CPU Time"             },
   { "   TIME+  ",  "%9.9s ",    9,    -1, _SF(P_TME), "CPU Time, hundredths" },
   { "%MEM ",       "%#4.1f ",  -1,    -1, _SF(P_RES), "Memory usage (RES)"   },
   { " VIRT ",      "%5.5s ",    5, SK_Kb, _SF(P_VRT), "Virtual Image (kb)"   },
   { "SWAP ",       "%4.4s ",    4, SK_Kb, _SF(P_SWP), "Swapped size (kb)"    },
   { " RES ",       "%4.4s ",    4, SK_Kb, _SF(P_RES), "Resident size (kb)"   },
   { "CODE ",       "%4.4s ",    4, SK_Kb, _SF(P_COD), "Code size (kb)"       },
   { "DATA ",       "%4.4s ",    4, SK_Kb, _SF(P_DAT), "Data+Stack size (kb)" },
   { " SHR ",       "%4.4s ",    4, SK_Kb, _SF(P_SHR), "Shared Mem size (kb)" },
   { "nFLT ",       "%4.4s ",    4, SK_no, _SF(P_FLT), "Page Fault count"     },
   { "nDRT ",       "%4.4s ",    4, SK_no, _SF(P_DRT), "Dirty Pages count"    },
#ifdef USE_LIB_STA3
   { "STA ",        "%3.3s ",   -1,    -1, _SF(P_STA), "Process Status"       },
#else
   { "S ",          "%c ",      -1,    -1, _SF(P_STA), "Process Status"       },
#endif
 /** next entry's special: '.head' will be formatted using table entry's own
                           '.fmts' plus runtime supplied conversion args! */
   { "Command ",    "%-*.*s ",  -1,    -1, _SF(P_CMD), "Command line/name"    },
   { "WCHAN     ",  "%-9.9s ",  -1,    -1, _SF(P_WCH), "Sleeping in Function" },
 /** next entry's special: the 0's will be replaced with '.'! */
#ifdef CASEUP_HEXES
   { "Flags    ",   "%08lX ",   -1,    -1, _SF(P_FLG), "Task Flags <sched.h>" }
#else
   { "Flags    ",   "%08lx ",   -1,    -1, _SF(P_FLG), "Task Flags <sched.h>" }
#endif
};


        /*
         * Display each field represented in the Fields Table along with its
         * description and mark (with a leading asterisk) fields associated
         * with upper case letter(s) in the passed 'fields string'.
         *
         * After all fields have been displayed, some extra explanatory
         * text may also be output */
static void display_fields (const char *fields, const char *xtra)
{
#define yRSVD 3
   const char *p;
   int i, cmax = Screen_cols / 2, rmax = Screen_rows - yRSVD;

   /* we're relying on callers to first clear the screen and thus avoid screen
      flicker if they're too lazy to handle their own asterisk (*) logic */
   putp(Cap_bold);
   for (i = 0; i < MAXTBL(Fieldstab); ++i) {
      int b = (NULL != strchr(fields, i + 'A'));
         /* advance past any leading spaces */
      for (p = Fieldstab[i].head; ' ' == *p; ++p)
         ;
      PUTP("%s%s%c %c: %-10s = %s"
         , tg2((i / rmax) * cmax, (i % rmax) + yRSVD)
         , b ? Cap_bold : Cap_norm
         , b ? '*' : ' '
         , b ? i + 'A' : i + 'a'
         , p
         , Fieldstab[i].desc);
   }
   if (xtra) {
      putp(Curwin->capclr_rownorm);
      while ((p = strchr(xtra, '\n'))) {
         ++i;
         PUTP("%s%.*s"
            , tg2((i / rmax) * cmax, (i % rmax) + yRSVD)
            , (int)(p - xtra)
            , xtra);
         xtra = ++p;
      }
   }
   putp(Caps_off);

#undef yRSVD
}


        /*
         * Change order of displayed fields. */
static void fields_reorder (void)
{
   static const char prompt[] =
      "Upper case letter moves field left, lower case right";
   char c, *p;
   int i;

   putp(Cap_clr_scr);
   putp(Cap_curs_huge);
   display_fields(Curwin->fieldscur, FIELDS_xtra);
   for (;;) {
      show_special(fmtmk(FIELDS_current
         , Cap_home, Curwin->fieldscur, Curwin->grpname, prompt));
      chin(0, &c, 1);
      i = toupper(c) - 'A';
      if (i < 0 || i >= MAXTBL(Fieldstab)) break;
      if (((p = strchr(Curwin->fieldscur, i + 'A')))
      || ((p = strchr(Curwin->fieldscur, i + 'a')))) {
         if (isupper(c)) p--;
         if (('\0' != p[1]) && (p >= Curwin->fieldscur)) {
            c    = p[0];
            p[0] = p[1];
            p[1] = c;
         }
      }
   }
   putp(Cap_curs_norm);
}

        /*
         * Select sort field. */
static void fields_sort (void)
{
   static const char prompt[] =
      "Select sort field via field letter, type any other key to return";
   char phoney[PFLAGSSIZ];
   char c, *p;
   int i, x;

   strcpy(phoney, NUL_FIELDS);
   x = i = Curwin->sortindx;
   putp(Cap_clr_scr);
   putp(Cap_curs_huge);
   for (;;) {
      p  = phoney + i;
      *p = toupper(*p);
      display_fields(phoney, SORT_xtra);
      show_special(fmtmk(SORT_fields
         , Cap_home, *p, Curwin->grpname, prompt));
      chin(0, &c, 1);
      i = toupper(c) - 'A';
      if (i < 0 || i >= MAXTBL(Fieldstab)) break;
      *p = tolower(*p);
      x = i;
   }
   if ((p = strchr(Curwin->fieldscur, x + 'a')))
      *p = x + 'A';
   Curwin->sortindx = x;
   putp(Cap_curs_norm);
}


        /*
         * Toggle displayed fields. */
static void fields_toggle (void)
{
   static const char prompt[] =
      "Toggle fields via field letter, type any other key to return";
   char c, *p;
   int i;

   putp(Cap_clr_scr);
   putp(Cap_curs_huge);
   for (;;) {
      display_fields(Curwin->fieldscur, FIELDS_xtra);
      show_special(fmtmk(FIELDS_current
         , Cap_home, Curwin->fieldscur, Curwin->grpname, prompt));
      chin(0, &c, 1);
      i = toupper(c) - 'A';
      if (i < 0 || i >= MAXTBL(Fieldstab)) break;
      if ((p = strchr(Curwin->fieldscur, i + 'A')))
         *p = i + 'a';
      else if ((p = strchr(Curwin->fieldscur, i + 'a')))
         *p = i + 'A';
   }
   putp(Cap_curs_norm);
}


/*######  Windows/Field Groups support  #################################*/

        /*
         * Set the number of fields/columns to display;
         * Create the field columns heading; and then
         * Set maximum cmdline length. */
static void win_colsheads (WIN_t *q)
{
   const char *h;
   int i, needpsdb = 0;

      /* build window's procflags array and establish a tentative maxpflgs */
   for (i = 0, q->maxpflgs = 0; q->fieldscur[i]; i++) {
      if (isupper(q->fieldscur[i]))
         q->procflags[q->maxpflgs++] = q->fieldscur[i] - 'A';
   }

      /* build a preliminary columns header not to exceed screen width
         (and account for a possible leading window number) */
   if (Mode_altscr) strcpy(q->columnhdr, " "); else q->columnhdr[0] = '\0';
   for (i = 0; i < q->maxpflgs; i++) {
      h = Fieldstab[q->procflags[i]].head;
         /* oops, won't fit -- we're outta here... */
      if (Screen_cols < (int)(strlen(q->columnhdr) + strlen(h))) break;
      strcat(q->columnhdr, h);
   }

      /* establish the final maxpflgs and prepare to grow the command
         column heading via maxcmdln -- it may be a fib if P_CMD wasn't
         encountered, but that's ok because it won't be displayed anyway */
   q->maxpflgs = i;
   q->maxcmdln = Screen_cols
      - (strlen(q->columnhdr) - strlen(Fieldstab[P_CMD].head)) - 1;

      /* now we can build the true run-time columns header and format the
         command column heading if P_CMD is really being displayed --
         show_a_task is aware of the addition of winnum to the header */
   snprintf(q->columnhdr, sizeof(q->columnhdr), "%s"
      , Mode_altscr ? fmtmk("%d", q->winnum) : "");
   for (i = 0; i < q->maxpflgs; i++) {
      h = Fieldstab[q->procflags[i]].head;
         /* are we gonna' need the kernel symbol table? */
      if (P_WCH == q->procflags[i]) needpsdb = 1;
      if (P_CMD == q->procflags[i])
         strcat(q->columnhdr
            , fmtmk(Fieldstab[P_CMD].fmts, q->maxcmdln, q->maxcmdln, h));
      else
         strcat(q->columnhdr, h);
   }

      /* do we need the kernel symbol table (and is it already open?) */
   if (needpsdb) {
      if (-1 == No_ksyms) {
         No_ksyms = 0;
         if (open_psdb_message(NULL, msg_save))
            /* why so counter-intuitive, couldn't open_psdb_message
               mirror sysmap_mmap -- that func does all the work anyway? */
            No_ksyms = 1;
         else
            PSDBopen = 1;
      }
   }
}


        /*
         * Tell caller if a specific pflag is 'exposing itself' (whoa!) */
static inline int win_fldviz (WIN_t *q, PFLG_t flg)
{
   PFLG_t *p = q->procflags + q->maxpflgs - 1;

   while (*p != flg && q->procflags < p) --p;
   return *p == flg;
}


        /*
         * Value a window's name and make the associated group name. */
static void win_names (WIN_t *q, const char *name)
{
   sprintf(q->winname, "%.*s", WINNAMSIZ -1, name);
   sprintf(q->grpname, "%d:%.*s", q->winnum, WINNAMSIZ -1, name);
}


        /*
         * Display a window/field group (ie. make it "current"). */
static void win_select (char ch)
{
   static const char prompt[] = "Choose field group (1 - 4)";

   /* if there's no ch, it means we're supporting the normal do_key routine,
      so we must try to get our own darn ch by begging the user... */
   if (!ch) {
      show_pmt(prompt);
      chin(0, (char *)&ch, 1);
   }
   switch (ch) {
      case 'a':                         /* we don't carry 'a' / 'w' in our */
         Curwin = Curwin->next;         /* pmt - they're here for a good   */
         break;                         /* friend of ours -- wins_colors.  */
      case 'w':                         /* (however those letters work via */
         Curwin = Curwin->prev;         /* the pmt too but gee, end-loser  */
         break;                         /* should just press the darn key) */
      case '1': case '2':
      case '3': case '4':
         Curwin = Winstk[ch - '1'];
         break;
   }
}


        /*
         * Just warn the user when a command can't be honored. */
static int win_warn (void)
{
   show_msg(fmtmk("\aCommand disabled, activate %s with '-' or '_'"
      , Curwin->grpname));
   /* we gotta' return false 'cause we're somewhat well known within
      macro society, by way of that sassy little tertiary operator... */
   return 0;
}


        /*
         * Change colors *Helper* function to save/restore settings;
         * ensure colors will show; and rebuild the terminfo strings. */
static void winsclr (WIN_t *q, int save)
{
   static int flgssav, summsav, msgssav, headsav, tasksav;

   if (save) {
      flgssav = q->winflags; summsav = q->summclr;
      msgssav = q->msgsclr;  headsav = q->headclr; tasksav = q->taskclr;
      SETw(q, Show_COLORS);
   } else {
      q->winflags = flgssav; q->summclr = summsav;
      q->msgsclr = msgssav;  q->headclr = headsav; q->taskclr = tasksav;
   }
   capsmk(q);
}


        /*
         * Change colors used in display */
static void wins_colors (void)
{
#define kbdABORT  'q'
#define kbdAPPLY  '\n'
   int clr = Curwin->taskclr, *pclr = &Curwin->taskclr;
   char ch, tgt = 'T';

   if (0 >= max_colors) {
      show_msg("\aNo colors to map!");
      return;
   }
   winsclr(Curwin, 1);
   putp(Cap_clr_scr);
   putp(Cap_curs_huge);

   do {
      putp(Cap_home);
         /* this string is well above ISO C89's minimum requirements! */
      show_special(fmtmk(COLOR_help
         , procps_version, Curwin->grpname
         , CHKw(Curwin, Show_HIBOLD) ? "On" : "Off"
         , CHKw(Curwin, Show_COLORS) ? "On" : "Off"
         , tgt, clr, Curwin->winname));
      chin(0, &ch, 1);
      switch (ch) {
         case 'S':
            pclr = &Curwin->summclr;
            clr = *pclr;
            tgt = ch;
            break;
         case 'M':
            pclr = &Curwin->msgsclr;
            clr = *pclr;
            tgt = ch;
            break;
         case 'H':
            pclr = &Curwin->headclr;
            clr = *pclr;
            tgt = ch;
            break;
         case 'T':
            pclr = &Curwin->taskclr;
            clr = *pclr;
            tgt = ch;
            break;
         case '0': case '1': case '2': case '3':
         case '4': case '5': case '6': case '7':
            clr = ch - '0';
            *pclr = clr;
            break;
         case 'b':
            TOGw(Curwin, Show_HIBOLD);
            break;
         case 'z':
            TOGw(Curwin, Show_COLORS);
            break;
         case 'a':
         case 'w':
            win_select(ch);
            winsclr(Curwin, 1);
            clr = Curwin->taskclr, pclr = &Curwin->taskclr;
            tgt = 'T';
            break;
      }
      capsmk(Curwin);
   } while (kbdAPPLY != ch && kbdABORT != ch);

   if (kbdABORT == ch)
      winsclr(Curwin, 0);
   putp(Cap_curs_norm);

#undef kbdABORT
#undef kbdAPPLY
}


        /*
         * Manipulate flag(s) for all our windows. */
static void wins_reflag (int what, int flg)
{
   WIN_t *w;

   w = Curwin;
   do {
      switch (what) {
         case Flags_TOG:
            TOGw(w, flg);
            break;
         case Flags_SET:                /* Ummmm, i can't find anybody */
            SETw(w, flg);               /* who uses Flags_set ...      */
            break;
         case Flags_OFF:
            OFFw(w, flg);
            break;
      }
         /* a flag with special significance -- user wants to rebalance
            display so we gotta' 'off' one number then force on two flags... */
      if (EQUWINS_cwo == flg) {
         w->maxtasks = 0;
         SETw(w, Show_IDLEPS | VISIBLE_tsk);
      }
      w = w->next;
   } while (w != Curwin);
}


        /*
         * Set the screen dimensions and call the real workhorse.
         * (also) catches:
         *    SIGWINCH and SIGCONT */
static void wins_resize (int dont_care_sig)
{
   struct winsize wz;
   WIN_t *w;

   (void)dont_care_sig;
   Screen_cols = columns;
   Screen_rows = lines;
   if (-1 != (ioctl(STDOUT_FILENO, TIOCGWINSZ, &wz))) {
      Screen_cols = wz.ws_col;
      Screen_rows = wz.ws_row;
   }
      /* we might disappoint some folks (but they'll deserve it) */
   if (SCREENMAX < Screen_cols) Screen_cols = SCREENMAX;

   w = Curwin;
   do {
      win_colsheads(w);
      w = w->next;
   } while (w != Curwin);
}


        /*
         * Set up the raw/incomplete field group windows --
         * they'll be finished off after startup completes.
         * [ and very likely that will override most/all of our efforts ]
         * [               --- life-is-NOT-fair ---                     ] */
static void windows_stage1 (void)
{
   static struct {
      const char *name;
      const char *flds;
      const int   sort;
      const int   clrs[4];      /* summ, msgs, heads, task */
   } wtab[] = {
      { "Def", DEF_FIELDS, P_CPU,
         { COLOR_RED, COLOR_RED, COLOR_YELLOW, COLOR_RED } },
      { "Job", JOB_FIELDS, P_PID,
         { COLOR_CYAN, COLOR_CYAN, COLOR_WHITE, COLOR_CYAN } },
      { "Mem", MEM_FIELDS, P_MEM,
         { COLOR_MAGENTA, COLOR_MAGENTA, COLOR_BLUE, COLOR_MAGENTA } },
      { "Usr", USR_FIELDS, P_USR,
         { COLOR_YELLOW, COLOR_YELLOW, COLOR_GREEN, COLOR_YELLOW } },
   };
   WIN_t *w;
   char *pc;
   int i, x, *pi;

      /* get all our window structs in one big chunk */
   w = alloc_c(sizeof(WIN_t) * GROUPSMAX);

   for (i = 0; i < GROUPSMAX; i++) {
      Winstk[i] = w;
      w->winnum = i + 1;
      strcpy(w->winname, wtab[i].name);
      strcpy(w->fieldscur, wtab[i].flds);
      w->sortindx = wtab[i].sort;
      w->winflags = DEF_WINFLGS;
      for (x = 0, pi = &w->summclr; x < 4; x++, pi++)
         *pi = wtab[i].clrs[x];
      w->captab[0] = Cap_norm;
      w->captab[1] = Cap_norm;
      w->captab[2] = Cap_bold;
         /* complete this win's captab, but not the brute force way... */
      for (x = 3, pc = w->capclr_sum; x < CAPTABMAX; x++) {
         w->captab[x] = pc;
         pc += CLRBUFSIZ;
      }
      w->next = w + 1;
      w->prev = w - 1;
      ++w;
   }
      /* fixup the circular chains... */
   Winstk[3]->next = Winstk[0];
   Winstk[0]->prev = Winstk[3];
   Curwin = Winstk[0];
   Mode_altscr = 0;
}


        /*
         * This guy just completes the field group windows after the
         * rcfiles have been read and command line arguments parsed
         * (he's also key to the success of that darn Batch mode). */
static void windows_stage2 (void)
{
   int i;

   if (Batch) {
      Mode_altscr = 0;
      OFFw(Curwin, Show_COLORS | Show_HICOLS | Show_HIROWS);
   }
   wins_resize(0);
   for (i = 0; i < GROUPSMAX; i++) {
      win_names(Winstk[i], Winstk[i]->winname);
      capsmk(Winstk[i]);
   }
}


/*######  Per-Frame Display support  #####################################*/

        /*
         * State display *Helper* function to calc and display the state
         * percentages for a single cpu.  In this way, we can support
         * the following environments without the usual code bloat.
         *    1 - single cpu machines
         *    2 - modest smp boxes with room for each cpu's percentages
         *    3 - massive smp guys leaving little or no room for process
         *        display and thus requiring the cpu summary toggle */
static void cpudo (CPUS_t *cpu, const char *pfx)
{
        /* we'll trim to zero if we get negative time ticks,
           which has happened with some SMP kernels (pre-2.4?) */
#define TRIMz(x)  ((tz = (STIC_t)x) < 0 ? 0 : tz)
   STIC_t u_frme, s_frme, n_frme, i_frme, w_frme, tot_frme, tz;
   float scale;

   u_frme = TRIMz(cpu->u - cpu->u_sav);
   s_frme = TRIMz(cpu->s - cpu->s_sav);
   n_frme = TRIMz(cpu->n - cpu->n_sav);
   i_frme = TRIMz(cpu->i - cpu->i_sav);
   w_frme = TRIMz(cpu->w - cpu->w_sav);
   tot_frme = u_frme + s_frme + n_frme + i_frme + w_frme;
   if (1 > tot_frme) tot_frme = 1;
   scale = 100.0 / (float)tot_frme;

      /* display some kinda' cpu state percentages
         (who or what is explained by the passed prefix) */
   show_special(fmtmk(States_fmts
      , pfx
      , (float)u_frme * scale
      , (float)s_frme * scale
      , (float)n_frme * scale
      , (float)i_frme * scale
      , (float)w_frme * scale));
   Msg_row += 1;

      /* remember for next time around */
   cpu->u_sav = cpu->u;
   cpu->s_sav = cpu->s;
   cpu->n_sav = cpu->n;
   cpu->i_sav = cpu->i;
   cpu->w_sav = cpu->w;

#undef TRIMz
}


        /*
         * Calc the number of tasks in each state (run, sleep, etc)
         * Prepare for the possible calculation of percent cpu usage (pcpu)
         * Calc the cpu(s) percent in each state (user, system, nice, idle)
         * AND establish the total number of tasks for this frame! */
static void frame_states (proc_t **ppt, int show)
{
   static HIST_t   *hist_sav;
   static HIST_t   *hist_new;
   static unsigned  hist_siz; // number of structs
   unsigned         total, running, sleeping, stopped, zombie;
   HIST_t          *hist_tmp;

   // reuse memory each time around
   hist_tmp = hist_sav;
   hist_sav = hist_new;
   hist_new = hist_tmp;

   total = running = sleeping = stopped = zombie = 0;
   time_elapsed();

      /* make a pass through the data to get stats */
   while (-1 != ppt[total]->pid) {                      /* calculations //// */
      TICS_t tics;
      proc_t *this = ppt[total];
      int i;

      switch (this->state) {
         case 'S':
         case 'D':
            sleeping++;
            break;
         case 'T':
            stopped++;
            break;
         case 'Z':
            zombie++;
            break;
         case 'R':
            running++;
            break;
      }
      if (total+1 >= hist_siz) {
         hist_siz = hist_siz * 5 / 4 + 100;  // grow by at least 25%
         hist_sav = alloc_r(hist_sav, sizeof(HIST_t) * hist_siz);
         hist_new = alloc_r(hist_new, sizeof(HIST_t) * hist_siz);
      }
         /* calculate time in this process; the sum of user time (utime)
            + system time (stime) -- but PLEASE dont waste time and effort on
            calcs and saves that go unused, like the old top! */
      hist_new[total].pid  = this->pid;
      hist_new[total].tics = tics = (this->utime + this->stime);

         /* find matching entry from previous pass and make ticks elapsed */
      for (i = 0; i < Frame_maxtask; i++) {
         if (this->pid == hist_sav[i].pid) {
            tics -= hist_sav[i].tics;
            break;
         }
      }
         /* we're just saving elapsed tics, to be converted into %cpu if
            this task wins it's displayable screen row lottery... */
      this->pcpu = tics;

      total++;
   } /* end: while 'pids' */

      /* shout results to the world (and us too, the next time around) */
   Frame_maxtask = total;

   if (show) {                                          /* display ///////// */
      static CPUS_t *smpcpu = NULL;

         /* display Task states */
      show_special(fmtmk(STATES_line1
         , total, running, sleeping, stopped, zombie));
      Msg_row += 1;

         /* refresh our /proc/stat data... */
      smpcpu = refreshcpus(smpcpu);

      if (CHKw(Curwin, View_CPUSUM)) {
            /* display just the 1st /proc/stat line */
         cpudo(&smpcpu[Cpu_tot], "Cpu(s):");
      } else {
         int i;
         char tmp[SMLBUFSIZ];
            /* display each cpu's states separately */
         for (i = 0; i < Cpu_tot; i++) {
            snprintf(tmp, sizeof(tmp), " Cpu%-2d:", Mode_irixps ? i : Cpu_map[i]);
            cpudo(&smpcpu[i], tmp);
         }
      }
   } /* end: if 'show' */
}


        /*
         * Obtain memory information and display it. */
static void frame_storage (void)
{
   meminfo();
   if (CHKw(Curwin, View_MEMORY)) {
      show_special(fmtmk(MEMORY_line1
         , kb_main_total, kb_main_used, kb_main_free, kb_main_buffers));
      show_special(fmtmk(MEMORY_line2
         , kb_swap_total, kb_swap_used, kb_swap_free, kb_main_cached));
      Msg_row += 2;
   }
}


        /*
         * Task display *Helper* function to handle highlighted
         * column transitions.  */
static void mkcol (WIN_t *q, PFLG_t idx, int sta, int *pad, char *buf, ...)
{
   char tmp[COLBUFSIZ];
   va_list va;

   va_start(va, buf);
      /* this conditional is for piece-of-mind only, it should NOT be needed
         given the macro employed by show_a_task (which calls us only when
         the target column is the current sort field and Show_HICOLS is on) */
   if (!CHKw(q, Show_HICOLS) || q->sortindx != idx) {
      vsprintf(buf, Fieldstab[idx].fmts, va);
   } else {
      vsnprintf(tmp, sizeof(tmp), Fieldstab[idx].fmts, va);
      sprintf(buf, "%s%s", q->capclr_rowhigh, tmp);
      *pad += q->len_rowhigh;
      if (!CHKw(q, Show_HIROWS) || 'R' != sta) {
         strcat(buf, q->capclr_rownorm);
         *pad += q->len_rownorm;
      }
   }
   va_end(va);
}


        /*
         * Display information for a single task row. */
static void show_a_task (WIN_t *q, proc_t *task)
{
   /* the following macro is our means to 'inline' emitting a column -- that's
      far and away the most frequent and costly part of top's entire job! */
#define MKCOL(q,idx,sta,pad,buf,arg...) do{ \
           if (!b) \
              snprintf(buf, sizeof(buf), f, ## arg); \
           else mkcol(q, idx, sta, pad, buf, ## arg); }while(0)

   char rbuf[ROWBUFSIZ];
   int j, x, pad;

      /* since win_colsheads adds a number to the window's column header,
         we must begin a row with that in mind... */
   pad = Mode_altscr;
   if (pad) strcpy(rbuf, " "); else rbuf[0] = '\0';

   for (x = 0; x < q->maxpflgs; x++) {
      char cbuf[COLBUFSIZ];
      char        a = task->state;              /* we'll use local var's so  */
      PFLG_t      i = q->procflags[x];          /* gcc doesn't reinvent the  */
      unsigned    s = Fieldstab[i].scale;       /* wheel -- yields a cryptic */
      unsigned    w = Fieldstab[i].width;       /* mkcol, but saves +1k code */
      const char *f = Fieldstab[i].fmts;        /* (this & next macro only) */
      int         b = (CHKw(q, Show_HICOLS) && q->sortindx == i);

      cbuf[0] = '\0';
      switch (i) {
         case P_CMD:
         {  char *cmdptr, cmdnam[ROWBUFSIZ];

            if (!CHKw(q, Show_CMDLIN))
               cmdptr = task->cmd;
            else {
               cmdnam[0] = '\0';
               if (task->cmdline) {
                  j = 0;
                  do {
                     /* during a kernel build, parts of the make will create
                        cmdlines in excess of 3000 bytes but *without* the
                        intervening nulls -- so we must limit our strcat... */
                     strcat(cmdnam
                        , fmtmk("%.*s ", q->maxcmdln, task->cmdline[j++]));
                     /* whoa, gnome's xscreensaver had a ^I in his cmdline
                        creating a line wrap when the window was maximized &
                        the tab came into view -- so whack those suckers... */
                     strim(1, cmdnam);
                     if (q->maxcmdln < (int)strlen(cmdnam)) break;
                  } while (task->cmdline[j]);
               } else {
                  /* if cmdline is absent, consider it a kernel thread and
                     display it uniquely (need sort callback's complicity) */
                  strcpy(cmdnam, fmtmk(CMDLINE_FMTS, task->cmd));
               }
               cmdptr = cmdnam;
            }
            MKCOL(q, i, a, &pad, cbuf, q->maxcmdln, q->maxcmdln, cmdptr);
         }
            break;
         case P_COD:
            MKCOL(q, i, a, &pad, cbuf, scale_num(PAGES_2K(task->trs), w, s));
            break;
         case P_CPN:
            MKCOL(q, i, a, &pad, cbuf, task->processor);
            break;
         case P_CPU:
         {  float u = (float)task->pcpu * Frame_tscale;

            if (99.9 < u) u = 99.9;
            MKCOL(q, i, a, &pad, cbuf, u);
         }
            break;
         case P_DAT:
            MKCOL(q, i, a, &pad, cbuf, scale_num(PAGES_2K(task->drs), w, s));
            break;
         case P_DRT:
            MKCOL(q, i, a, &pad, cbuf, scale_num((unsigned)task->dt, w, s));
            break;
         case P_FLG:
            MKCOL(q, i, a, &pad, cbuf, (long)task->flags);
            for (j = 0; cbuf[j]; j++)
               if ('0' == cbuf[j]) cbuf[j] = '.';
            break;
         case P_FLT:
            MKCOL(q, i, a, &pad, cbuf, scale_num(task->maj_flt, w, s));
            break;
         case P_GRP:
            MKCOL(q, i, a, &pad, cbuf, task->egroup);
            break;
         case P_MEM:
            MKCOL(q, i, a, &pad, cbuf
               , (float)PAGES_2K(task->resident) * 100 / kb_main_total);
            break;
         case P_NCE:
            MKCOL(q, i, a, &pad, cbuf, (long)task->nice);
            break;
         case P_PGD:
            MKCOL(q, i, a, &pad, cbuf, task->pgrp);
            break;
         case P_PID:
            MKCOL(q, i, a, &pad, cbuf, task->pid);
            break;
         case P_PPD:
            MKCOL(q, i, a, &pad, cbuf, task->ppid);
            break;
         case P_PRI:
               /* quick & dirty response to 2.5.xx RT priority */
            if (-99 > task->priority) task->priority = -99;
            else if (+99 < task->priority) task->priority = +99;
            MKCOL(q, i, a, &pad, cbuf, (long)task->priority);
            break;
         case P_RES:
            MKCOL(q, i, a, &pad, cbuf, scale_num(PAGES_2K(task->resident), w, s));
            break;
         case P_SHR:
            MKCOL(q, i, a, &pad, cbuf, scale_num(PAGES_2K(task->share), w, s));
            break;
         case P_STA:
#ifdef USE_LIB_STA3
            MKCOL(q, i, a, &pad, cbuf, status(task));
#else
            MKCOL(q, i, a, &pad, cbuf, task->state);
#endif
            break;
         case P_SWP:
            MKCOL(q, i, a, &pad, cbuf
               , scale_num(PAGES_2K(task->size - task->resident), w, s));
            break;
         case P_TME:
         case P_TM2:
         {  TICS_t t;

            t = task->utime + task->stime;
            if (CHKw(q, Show_CTIMES))
               t += (task->cutime + task->cstime);
            MKCOL(q, i, a, &pad, cbuf, scale_tics(t, w));
         }
            break;
         case P_TTY:
         {  char tmp[TNYBUFSIZ];

            dev_to_tty(tmp, (int)w, task->tty, task->pid, ABBREV_DEV);
            MKCOL(q, i, a, &pad, cbuf, tmp);
         }
            break;
         case P_UID:
            MKCOL(q, i, a, &pad, cbuf, task->euid);
            break;
         case P_USR:
            MKCOL(q, i, a, &pad, cbuf, task->euser);
            break;
         case P_VRT:
            MKCOL(q, i, a, &pad, cbuf, scale_num(PAGES_2K(task->size), w, s));
            break;
         case P_WCH:
            if (No_ksyms) {
#ifdef CASEUP_HEXES
               MKCOL(q, i, a, &pad, cbuf, fmtmk("x%08lX", (long)task->wchan));
#else
               MKCOL(q, i, a, &pad, cbuf, fmtmk("x%08lx", (long)task->wchan));
#endif
            } else {
               MKCOL(q, i, a, &pad, cbuf, wchan(task->wchan));
            }
            break;

        } /* end: switch 'procflag' */

        strcat(rbuf, cbuf);
   } /* end: for 'maxpflgs' */

   /* This row buffer could be stuffed with parameterized strings... */
   PUTP("\n%s%.*s%s%s", (CHKw(q, Show_HIROWS) && 'R' == task->state)
      ? q->capclr_rowhigh : q->capclr_rownorm
      , Screen_cols + pad
      , rbuf
      , Caps_off
      , Cap_clr_eol);

#undef MKCOL
}


/*######  Main Screen routines  ##########################################*/

        /*
         * Process keyboard input during the main loop */
static void do_key (unsigned c)
{
      /* standardized 'secure mode' errors */
   static const char err_secure[] = "\aUnavailable in secure mode";
#ifdef WARN_NOT_SMP
      /* standardized 'smp' errors */
   static const char err_smp[] = "\aSorry, only 1 cpu detected";
#endif

   switch (c) {
      case '1':
#ifdef WARN_NOT_SMP
         if (Cpu_tot > 1)
            TOGw(Curwin, View_CPUSUM);
         else
            show_msg(err_smp);
#else
         TOGw(Curwin, View_CPUSUM);
#endif
         break;

      case 'a':
         if (Mode_altscr) Curwin = Curwin->next;
         break;

      case 'A':
         Mode_altscr = !Mode_altscr;
         wins_resize(0);
         break;

      case 'b':
         if (VIZCHKc) {
            if (!CHKw(Curwin, Show_HICOLS) && !CHKw(Curwin, Show_HIROWS))
               show_msg("\aNothing to highlight!");
            else {
               TOGw(Curwin, Show_HIBOLD);
               capsmk(Curwin);
            }
         }
         break;

      case 'c':
         VIZTOGc(Show_CMDLIN);
         break;

      case 'd':
      case 's':
         if (Secure_mode)
            show_msg(err_secure);
         else {
            float tmp =
               get_float(fmtmk("Change delay from %.1f to", Delay_time));
            if (tmp > -1) Delay_time = tmp;
         }
         break;

      case 'f':
         if (VIZCHKc) {
            fields_toggle();
            win_colsheads(Curwin);
         }
         break;

      case 'F':
      case 'O':
         if (VIZCHKc) {
            fields_sort();
            win_colsheads(Curwin);
         }
         break;

      case 'g':
         if (Mode_altscr) {
            char tmp[GETBUFSIZ];
            strcpy(tmp, ask4str(fmtmk("Rename window '%s' to (1-3 chars)"
               , Curwin->winname)));
            if (tmp[0]) win_names(Curwin, tmp);
         }
         break;

      case 'G':
         win_select(0);
         break;

      case 'h':
      case '?':
      {  char ch;

         putp(Cap_clr_scr);
         putp(Cap_curs_huge);
            /* this string is well above ISO C89's minimum requirements! */
         show_special(fmtmk(KEYS_help
            , procps_version
            , Curwin->grpname
            , CHKw(Curwin, Show_CTIMES) ? "On" : "Off"
            , Delay_time
            , Secure_mode ? "On" : "Off"
            , Secure_mode ? "" : KEYS_help_unsecured));
         chin(0, &ch, 1);
         if ('?' == ch || 'h' == ch) {
            do {
               putp(Cap_clr_scr);
               show_special(fmtmk(WINDOWS_help
                  , Curwin->grpname
                  , Winstk[0]->winname
                  , Winstk[1]->winname
                  , Winstk[2]->winname
                  , Winstk[3]->winname));
               chin(0, &ch, 1);
               win_select(ch);
            } while ('\n' != ch);
         }
         putp(Cap_curs_norm);
      }
         break;

      case 'i':
         VIZTOGc(Show_IDLEPS);
         break;

      case 'I':
#ifdef WARN_NOT_SMP
         if (Cpu_tot > 1) {
            Mode_irixps = !Mode_irixps;
            show_msg(fmtmk("Irix mode %s", Mode_irixps ? "On" : "Off"));
         } else
            show_msg(err_smp);
#else
         Mode_irixps = !Mode_irixps;
         show_msg(fmtmk("Irix mode %s", Mode_irixps ? "On" : "Off"));
#endif
         break;

      case 'k':
         if (Secure_mode) {
            show_msg(err_secure);
         } else {
            int sig, pid = get_int("PID to kill");

            if (-1 != pid) {
               sig = signal_name_to_number(
                  ask4str(fmtmk("Kill PID %d with signal [%i]"
                     , pid, DEF_SIGNAL)));
               if (-1 == sig) sig = DEF_SIGNAL;
               if (sig && kill(pid, sig))
                  show_msg(fmtmk("\aKill of PID '%d' with '%d' failed: %s"
                     , pid, sig, strerror(errno)));
            }
         }
         break;

      case 'l':
         TOGw(Curwin, View_LOADAV);
         break;

      case 'm':
         TOGw(Curwin, View_MEMORY);
         break;

      case 'n':
      case '#':
         if (VIZCHKc) {
            int num;
            if (-1 < (num = get_int(
               fmtmk("Maximum tasks = %d, change to (0 is unlimited)"
                  , Curwin->maxtasks))))
               Curwin->maxtasks = num;
         }
         break;

      case 'o':
         if (VIZCHKc) {
            fields_reorder();
            win_colsheads(Curwin);
         }
         break;

      case 'q':
         stop(0);

      case 'r':
         if (Secure_mode)
            show_msg(err_secure);
         else {
            int pid, val;

            pid = get_int("PID to renice");
            if (-1 == pid) break;
            val = get_int(fmtmk("Renice PID %d to value", pid));
            if (setpriority(PRIO_PROCESS, (unsigned)pid, val))
               show_msg(fmtmk("\aRenice of PID %d to %d failed: %s"
                  , pid, val, strerror(errno)));
         }
         break;

      case 'R':
         VIZTOGc(Qsrt_NORMAL);
         break;

      case 'S':
         if (VIZCHKc) {
            TOGw(Curwin, Show_CTIMES);
            show_msg(fmtmk("Cumulative time %s"
               , CHKw(Curwin, Show_CTIMES) ? "On" : "Off"));
         }
         break;

      case 't':
         TOGw(Curwin, View_STATES);
         break;

      case 'u':
         if (VIZCHKc)
            strcpy(Curwin->colusrnam, ask4str("Which user (blank for all)"));
         break;

      case 'w':
         if (Mode_altscr) Curwin = Curwin->prev;
         break;

      case 'W':
      {  FILE *fp = fopen(RCfile, "w"); int i;

         if (fp) {
            fprintf(fp, "RCfile for \"%s with windows\"\t\t# shameless braggin'\n"
               , Myname);
            fprintf(fp, "Id:%c, "
               "Mode_altscr=%d, Mode_irixps=%d, Delay_time=%.3f, Curwin=%d\n"
               , RCF_FILEID
               , Mode_altscr, Mode_irixps, Delay_time, Curwin - Winstk[0]);
            for (i = 0; i < GROUPSMAX; i++) {
               fprintf(fp, "%s\tfieldscur=%s\n"
                  , Winstk[i]->winname, Winstk[i]->fieldscur);
               fprintf(fp, "\twinflags=%d, sortindx=%d, maxtasks=%d\n"
                  , Winstk[i]->winflags
                  , Winstk[i]->sortindx
                  , Winstk[i]->maxtasks);
               fprintf(fp, "\tsummclr=%d, msgsclr=%d, headclr=%d, taskclr=%d\n"
                  , Winstk[i]->summclr
                  , Winstk[i]->msgsclr
                  , Winstk[i]->headclr
                  , Winstk[i]->taskclr);
            }
            fclose(fp);
            show_msg(fmtmk("Wrote configuration to '%s'", RCfile));
         } else
            show_msg(fmtmk("\aFailed '%s' open: %s", RCfile, strerror(errno)));
      }
         break;

      case 'x':
         if (VIZCHKc) {
            TOGw(Curwin, Show_HICOLS);
            capsmk(Curwin);
         }
         break;

      case 'y':
         if (VIZCHKc) {
            TOGw(Curwin, Show_HIROWS);
            capsmk(Curwin);
         }
         break;

      case 'z':
         if (VIZCHKc) {
            TOGw(Curwin, Show_COLORS);
            capsmk(Curwin);
         }
         break;

      case 'Z':
         wins_colors();
         break;

      case '-':
         if (Mode_altscr)
            TOGw(Curwin, VISIBLE_tsk);
         break;

      case '_':
         if (Mode_altscr)
            wins_reflag(Flags_TOG, VISIBLE_tsk);
         break;

      case '=':
         Curwin->maxtasks = 0;
         SETw(Curwin, Show_IDLEPS | VISIBLE_tsk);
         Monpidsidx = 0;
         break;

      case '+':
         if (Mode_altscr)
            SETw(Curwin, EQUWINS_cwo);
         break;

      case '<':
         if (VIZCHKc) {
            PFLG_t *p = Curwin->procflags + Curwin->maxpflgs - 1;
            while (*p != Curwin->sortindx)
               --p;
            if (--p >= Curwin->procflags)
               Curwin->sortindx = *p;
         }
         break;

      case '>':
         if (VIZCHKc) {
            PFLG_t *p = Curwin->procflags;
            while (*p != Curwin->sortindx)
               ++p;
            if (++p < Curwin->procflags + Curwin->maxpflgs)
               Curwin->sortindx = *p;
         }
         break;

      case '\n':          /* just ignore these, they'll have the effect */
      case ' ':           /* of refreshing display after waking us up ! */
         break;

      default:
         show_msg("\aUnknown command - try 'h' for help");
   }
}


        /*
         * Begin a new frame by:
         *    1) Refreshing the all important proc table
         *    2) Displaying uptime and load average (maybe)
         *    3) Arranging for task/cpu states to be displayed
         *    4) Arranging for memory & swap usage to be displayed
         * and then, returning a pointer to the pointers to the proc_t's! */
static proc_t **do_summary (void)
{
   static proc_t **p_table = NULL;
   int p_flags = PROC_FILLMEM | PROC_FILLSTAT | PROC_FILLSTATUS;
   WIN_t *w;

      /* first try to minimize the cost of this frame (cross your fingers) */
   w = Curwin;
   do {
      if (!Mode_altscr || CHKw(w, VISIBLE_tsk)) {
         p_flags |= (CHKw(w, Show_CMDLIN) && win_fldviz(w, P_CMD)) ? PROC_FILLCOM : 0;
         p_flags |= win_fldviz(w, P_USR) ? PROC_FILLUSR : 0;
         p_flags |= win_fldviz(w, P_GRP) ? PROC_FILLGRP : 0;
      }
      if (Mode_altscr) w = w->next;
   } while (w != Curwin);

   if (!p_table) {
         /* whoa first time, gotta' prime the pump... */
      p_table = refreshprocs(NULL, p_flags);
      frame_states(p_table, 0);
      putp(Cap_clr_scr);
      sleep(1);
   } else
      putp(Batch ? "\n\n" : Cap_home);


      /*
       ** Display Load averages */
   if (CHKw(Curwin, View_LOADAV)) {
      if (!Mode_altscr)
         show_special(fmtmk(LOADAV_line, Myname, sprint_uptime()));
      else
         show_special(fmtmk(CHKw(Curwin, VISIBLE_tsk)
            ? LOADAV_line_alt
            : LOADAV_line
            , Curwin->grpname, sprint_uptime()));
      Msg_row += 1;
   }

      /*
       ** Display Tasks and Cpu(s) states and also prime for potential 'pcpu',
       ** but NO table sort yet -- that's done on a per window basis! */
   p_table = refreshprocs(p_table, p_flags);
   frame_states(p_table, CHKw(Curwin, View_STATES));

      /*
       ** Display Memory and Swap space usage */
   frame_storage();

#ifndef YIELDCPU_OFF
   /* jeeze pucker up, it's time to kiss the scheduler's butt...

      Alright Mr. Kernel, that's ENOUGH already.  This swell little program
      is SICK and TIRED of being PUNISHED for its CAREFUL USE of cpu cycles
      (quite unlike old top who just threw them away).  You constantly make
      me FIGHT my way back up the RUN-QUEUE!  Dammit, I am GOOD, regardless
      of whether your GOODNESS says so.  So here's the deal: I'll yield the
      darn cpu, if you'll promise to re-dispatch me real soon, ok? */
   sched_yield();
#endif
   SETw(Curwin, NEWFRAM_cwo);
   return p_table;

#undef myCMD
#undef myGRP
}


        /*
         * Squeeze as many tasks as we can into a single window,
         * after sorting the passed proc table. */
static void do_window (proc_t **ppt, WIN_t *q, int *lscr)
{
#ifdef SORT_SUPRESS
   /* the 1 flag that DOES and 2 flags that MAY impact our proc table qsort */
#define srtMASK  ~( Qsrt_NORMAL | Show_CMDLIN | Show_CTIMES )
   static PFLG_t sav_indx = 0;
   static int    sav_flgs = -1;
#endif
   int i, lwin;

      /*
       ** Display Column Headings -- and distract 'em while we sort (maybe) */
   PUTP("\n%s%s%s%s", q->capclr_hdr, q->columnhdr, Caps_off, Cap_clr_eol);

#ifdef SORT_SUPRESS
   if (CHKw(Curwin, NEWFRAM_cwo)
   || sav_indx != q->sortindx
   || sav_flgs != (q->winflags & srtMASK)) {
      sav_indx = q->sortindx;
      sav_flgs = (q->winflags & srtMASK);
#endif
                                                /* this one's always needed! */
      if (CHKw(q, Qsrt_NORMAL)) Frame_srtflg = 1;
         else Frame_srtflg = -1;
      Frame_ctimes = CHKw(q, Show_CTIMES);      /* this and next, only maybe */
      Frame_cmdlin = CHKw(q, Show_CMDLIN);
      qsort(ppt, (unsigned)Frame_maxtask, sizeof(proc_t *)
         , Fieldstab[q->sortindx].sort);
#ifdef SORT_SUPRESS
   }
#endif
      /* account for column headings */
   if (!Batch) (*lscr)++;
   lwin = 1;
   i = 0;

   while ( -1 != ppt[i]->pid && *lscr < Max_lines
   &&  (!q->winlines || (lwin <= q->winlines)) ) {
      if ((CHKw(q, Show_IDLEPS)
      || ('S' != ppt[i]->state && 'Z' != ppt[i]->state))
      && ((!q->colusrnam[0])
      || (!strcmp(q->colusrnam, ppt[i]->euser)) ) ) {
            /*
             ** Display a process Row */
         show_a_task(q, ppt[i]);
         if (!Batch) (*lscr)++;
         ++lwin;
      }
      ++i;
   }
      /* for this frame that window's toast, cleanup for next time */
   q->winlines = 0;
   OFFw(Curwin, FLGSOFF_cwo);

#ifdef SORT_SUPRESS
#undef srtMASK
#endif
}


        /*
         * This guy's just a *Helper* function who apportions the
         * remaining amount of screen real estate under multiple windows
         * -- i swear that's the whole truth, so-help-me ! */
static void sohelpme (int wix, int max)
{
   int i, rsvd, size, wins;

      /* calc remaining number of visible windows + total 'user' lines */
   for (i = wix, rsvd = 0, wins = 0; i < GROUPSMAX; i++) {
      if (CHKw(Winstk[i], VISIBLE_tsk)) {
         rsvd += Winstk[i]->maxtasks;
         ++wins;
         if (max <= rsvd) break;
      }
   }
   if (!wins) wins = 1;
      /* set aside 'rsvd' & deduct 1 line/window for the columns heading */
   size = (max - wins) - rsvd;
   if (0 <= size) size = max;
   size = (max - wins) / wins;

      /* for remaining windows, set do_window's winlines to either the
         user's maxtask (1st choice) or our 'foxized' size calculation
         (foxized  adj. -  'fair and balanced') */
   for (i = wix ; i < GROUPSMAX; i++) {
      if (CHKw(Winstk[i], VISIBLE_tsk)) {
         Winstk[i]->winlines =
            Winstk[i]->maxtasks ? Winstk[i]->maxtasks : size;
      }
   }
}


        /*
         * Initiate the Frame Display Update cycle at someone's whim!
         * This routine doesn't do much, mostly he just calls others.
         *
         * (Whoa, wait a minute, we DO caretake that Msg_row guy and)
         * (we CALCULATE that IMPORTANT Max_lines thingy so that the)
         * (*subordinate* functions invoked know WHEN the user's had)
         * (ENOUGH already.  And at Frame End, it SHOULD be apparent)
         * (WE am d'MAN -- clearing UNUSED screen LINES and ensuring)
         * (the CURSOR is STUCK in just the RIGHT place, know what I)
         * (mean?  Huh, "doesn't DO MUCH"!  Never, EVER think or say)
         * (THAT about THIS function again, Ok?  Good that's better.)
         *
         * (ps. we ARE the UNEQUALED justification KING of COMMENTS!)
         * (No, I don't mean significance/relevance, only alignment.)
         *
         * (What's that?  Are you sure?  Old main's REALLY GOOD too?)
         * (You say he even JUSTIFIES comments in his FUNCTION BODY?)
         * (Jeeze, how COULD I have known?  That sob's NOT IN SCOPE!)
         */
static void so_lets_see_em (void)
{
   proc_t **ppt;
   int i, scrlins;

   Msg_row = scrlins = 0;
   ppt = do_summary();
   Max_lines = (Screen_rows - Msg_row) - 1;

   if (CHKw(Curwin, EQUWINS_cwo))
      wins_reflag(Flags_OFF, EQUWINS_cwo);

      /* sure hope each window's columns header begins with a newline... */
   putp(tg2(0, Msg_row));

   if (!Mode_altscr) {
         /* only 1 window to show so, piece o' cake */
      Curwin->winlines = Curwin->maxtasks;
      do_window(ppt, Curwin, &scrlins);
   } else {
         /* maybe NO window is visible but assume, pieces o' cakes */
      for (i = 0 ; i < GROUPSMAX; i++) {
         if (CHKw(Winstk[i], VISIBLE_tsk)) {
            sohelpme(i, Max_lines - scrlins);
            do_window(ppt, Winstk[i], &scrlins);
         }
         if (Max_lines <= scrlins) break;
      }
   }
   /* clear to end-of-screen (critical if last window is 'idleps off'),
      then put the cursor in-its-place, and rid us of any prior frame's msg
      (main loop must iterate such that we're always called before sleep) */
   PUTP("%s%s%s", Cap_clr_eos, tg2(0, Msg_row), Cap_clr_eol);
   fflush(stdout);
}


/*######  Entry point  ###################################################*/

        /*
         * Darling, you DO look simply MARVELOUS -- have you been dieting?
         * Or maybe it's because YOU and WINDOWS seem such a HAPPY couple.
         *
         * Of course NO.  Not THOSE deathly BLUE WINDOWS!  I mean your 'A'
         * mode (alt display) windows.  Yes, yes those completely OPTIONAL
         * ones.  We ALL know you'd NEVER FORCE that interface on ANY user
         * - unlike You-Know-Who!  Well I've got to run.  But you're doing
         * it just SPLENDIDLY!  You go right on doing it EXACTLY the SAME!
         */
int main (int dont_care_argc, char **argv)
{
   //(void)dont_care_argc;
   before(*argv);
  /*
   Ok, she's gone now.  Don't you mind her, she means well but yes, she is
   a bit of a busy-body.  Always playing the matchmaker role, trying to do
   away with unmarried windows and bachelors.  So, back to business buddy!

   You're hungry, you said?  How'd you like a sandwich?  No, no, no -- not
   the usual slopped together, hacked up illogic.  I'm talkin' a carefully
   reasoned, artfully crafted, extremely capable, well behaved executable!

   Well then, here, try THIS sandwich...
                                                           +-------------+ */
   windows_stage1();                                    /* top (sic) slice */
   configs_read();                                      /* > spread etc, < */
   //parse_args(&argv[1]);                              /* > lean stuff, < */
   process_command_line_arguments(&dont_care_argc, argv);
   whack_terminal();                                    /* > onions etc. < */
   windows_stage2();                                    /* as bottom slice */
  /*                                                       +-------------+ */
   signal(SIGALRM,  stop);
   signal(SIGHUP,   stop);
   signal(SIGINT,   stop);
   signal(SIGPIPE,  stop);
   signal(SIGQUIT,  stop);
   signal(SIGTERM,  stop);
   signal(SIGTSTP,  suspend);
   signal(SIGTTIN,  suspend);
   signal(SIGTTOU,  suspend);
   signal(SIGCONT,  wins_resize);
   signal(SIGWINCH, wins_resize);

   for (;;) {
      struct timeval tv;
      fd_set fs;
      char c;
                                                           /*  This is it? */
      so_lets_see_em();                                    /*  Impossible! */

      if (Msg_awaiting) show_msg(Msg_delayed);
      if (0 < Loops) --Loops;
      if (!Loops) stop(0);

      if (Batch)
         sleep((unsigned)Delay_time);
      else {                             /*  Linux reports time not slept, */
         tv.tv_sec = Delay_time;         /*  so we must reinit every time. */
         tv.tv_usec = (Delay_time - (int)Delay_time) * 1000000;
         FD_ZERO(&fs);
         FD_SET(STDIN_FILENO, &fs);
         if (0 < select(STDIN_FILENO+1, &fs, NULL, NULL, &tv)
         &&  0 < chin(0, &c, 1))
            do_key((unsigned)c);
      }
   }

  /*
   (listen before we return, aren't you sort of sad for 'so_lets_see-em'?)
   (so, uh, why don't we just move this main guy to near the beginning of)
   (the C source file.  then that poor old function would be sure to have)
   (at least a chance at scopin' us out, ya know what i mean?  so what do)
   (ya think?  all things considered, would that be a proper thing to do?)

   Now there's an EXCELLENT idea!  After all, SOME programmers DO code the
   main function ANY OLD PLACE they feel like.  And in doing THAT, they're
   helping to keep those that FOLLOW out of mischief, busy HUNTING for the
   <bleepin'> thing.  Further, by moving it we can contribute to PROTOTYPE
   PROLIFERATION for every function main calls.  Don't you KNOW that those
   declarations OFTEN LINGER, usually long AFTER the REAL definitions have
   DISAPPEARED, since programs do evolve?  Yep that's a GREAT idea you got
   there, NICE GOING!  But, here's an opposing view: ANYONE who'd put main
   ANYWHERE such that its LOCATION cannot be REACHED with JUST 1 KEYSTROKE
   better turn in their Coding_Badge and Pocket_Protector then -- BE GONE!
   The main function has only ONE proper HOME: always the LAST function in
   that C Listing.  End-of-Story, No-More-Discussion, so BE QUIET already!

   \---------------------------------------------------------------------/
   Sheeesh, didn't that dufus know the return statement can't be executed,
   or we end via that stop() function?  Oh Lordy, I is DROWNING in morons;
   they done REACHED clear up to my OUTER braces!  We's surely DOOMED now!
   /---------------------------------------------------------------------\
  */
   return 0;
}
