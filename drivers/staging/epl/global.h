/****************************************************************************

    global project definition file

    12.06.1998   -rs
    11.02.2002   r.d. Erweiterungen, Ergaenzungen
    20.08.2002   SYS TEC electronic -as
                 Definition Schluesselwort 'GENERIC'
                 fuer das Erzeugen von Generic Pointer
    28.08.2002   r.d. erweiterter SYS TEC Debug Code
    16.09.2002   r.d. komplette Uebersetzung in Englisch
    11.04.2003   f.j. Ergaenzung fuer Mitsubishi NC30 Compiler
    17.06.2003   -rs  Definition von Basistypen in <#ifndef _WINDEF_> gesetzt
    16.04.2004   r.d. Ergaenzung fuer Borland C++ Builder
    30.08.2004   -rs  TRACE5 eingefügt
    23.12.2005   d.k. Definitions for IAR compiler

    $Id: global.h,v 1.6 2008/11/07 13:55:56 D.Krueger Exp $

****************************************************************************/

#ifndef _GLOBAL_H_
#define _GLOBAL_H_


#ifndef QWORD
#define QWORD long long int
#endif

    // ------------------ GNUC for I386 ---------------------------------------------

#define ROM			// code or variables mapped to ROM (i.e. flash)
					// usage: CONST BYTE ROM foo = 0x00;
#define HWACC			// hardware access through external memory (i.e. CAN)

#define MEM			// Memory attribute to optimize speed and code of pointer access.

#ifndef FAR
#define FAR			// variables mapped to external data storage location
#endif

#ifndef CONST
#define CONST const		// variables mapped to ROM (i.e. flash)
#endif

#define REENTRANT

#define TRACE  printk

#ifndef SHORT
#define SHORT short int
#endif
#ifndef USHORT
#define USHORT unsigned short int
#endif
#ifndef INT
#define INT int
#endif
#ifndef UINT
#define UINT unsigned int
#endif
#ifndef LONG
#define LONG long int
#endif
#ifndef ULONG
#define ULONG unsigned long int
#endif
    // --- logic types ---
#ifndef BYTE
#define BYTE unsigned char
#endif
#ifndef WORD
#define WORD unsigned short int
#endif
#ifndef DWORD
#define DWORD unsigned long int
#endif
#ifndef BOOL
#define BOOL unsigned char
#endif
    // --- alias types ---
#ifndef TRUE
#define TRUE  0xFF
#endif
#ifndef FALSE
#define FALSE 0x00
#endif
#ifndef _TIME_OF_DAY_DEFINED_
typedef struct {
	unsigned long int m_dwMs;
	unsigned short int m_wDays;

} tTimeOfDay;

#define _TIME_OF_DAY_DEFINED_

#endif

//---------------------------------------------------------------------------
//  Definition von TRACE
//---------------------------------------------------------------------------

#ifndef NDEBUG

#ifndef TRACE0
#define TRACE0(p0)                      TRACE(p0)
#endif

#ifndef TRACE1
#define TRACE1(p0, p1)                  TRACE(p0, p1)
#endif

#ifndef TRACE2
#define TRACE2(p0, p1, p2)              TRACE(p0, p1, p2)
#endif

#ifndef TRACE3
#define TRACE3(p0, p1, p2, p3)          TRACE(p0, p1, p2, p3)
#endif

#ifndef TRACE4
#define TRACE4(p0, p1, p2, p3, p4)      TRACE(p0, p1, p2, p3, p4)
#endif

#ifndef TRACE5
#define TRACE5(p0, p1, p2, p3, p4, p5)  TRACE(p0, p1, p2, p3, p4, p5)
#endif

#ifndef TRACE6
#define TRACE6(p0, p1, p2, p3, p4, p5, p6)  TRACE(p0, p1, p2, p3, p4, p5, p6)
#endif

#else

#ifndef TRACE0
#define TRACE0(p0)
#endif

#ifndef TRACE1
#define TRACE1(p0, p1)
#endif

#ifndef TRACE2
#define TRACE2(p0, p1, p2)
#endif

#ifndef TRACE3
#define TRACE3(p0, p1, p2, p3)
#endif

#ifndef TRACE4
#define TRACE4(p0, p1, p2, p3, p4)
#endif

#ifndef TRACE5
#define TRACE5(p0, p1, p2, p3, p4, p5)
#endif

#ifndef TRACE6
#define TRACE6(p0, p1, p2, p3, p4, p5, p6)
#endif

#endif

//---------------------------------------------------------------------------
//  definition of ASSERT
//---------------------------------------------------------------------------

#ifndef ASSERT
#define ASSERT(p)
#endif

//---------------------------------------------------------------------------
//  SYS TEC extensions
//---------------------------------------------------------------------------

// This macro doesn't print out C-file and line number of the failed assertion
// but a string, which exactly names the mistake.
#ifndef NDEBUG

#define ASSERTMSG(expr,string)  if (!(expr)) {\
                                        PRINTF0 ("Assertion failed: " string );\
                                        while (1);}
#else
#define ASSERTMSG(expr,string)
#endif

//---------------------------------------------------------------------------

#endif // #ifndef _GLOBAL_H_

// Please keep an empty line at the end of this file.
