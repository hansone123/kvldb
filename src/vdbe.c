/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** The code in this file implements execution method of the 
** Virtual Database Engine (VDBE).  A separate file ("vdbeaux.c")
** handles housekeeping details such as creating and deleting
** VDBE instances.  This file is solely interested in executing
** the VDBE program.
**
** In the external interface, an "sqlite4_stmt*" is an opaque pointer
** to a VDBE.
**
** The SQL parser generates a program which is then executed by
** the VDBE to do the work of the SQL statement.  VDBE programs are 
** similar in form to assembly language.  The program consists of
** a linear sequence of operations.  Each operation has an opcode 
** and 5 operands.  Operands P1, P2, and P3 are integers.  Operand P4 
** is a null-terminated string.  Operand P5 is an unsigned character.
** Few opcodes use all 5 operands.
**
** Computation results are stored on a set of registers numbered beginning
** with 1 and going up to Vdbe.nMem.  Each register can store
** either an integer, a null-terminated string, a floating point
** number, or the SQL "NULL" value.  An implicit conversion from one
** type to the other occurs as necessary.
** 
** Most of the code in this file is taken up by the sqlite4VdbeExec()
** function which does the work of interpreting a VDBE program.
** But other routines are also provided to help in building up
** a program instruction by instruction.
**
** Various scripts scan this source file in order to generate HTML
** documentation, headers files, or other derived files.  The formatting
** of the code in this file is, therefore, important.  See other comments
** in this file for details.  If in doubt, do not deviate from existing
** commenting and indentation practices when changing or adding code.
*/
#include "sqliteInt.h"
#include "vdbeInt.h"

/*
** Invoke this macro on memory cells just prior to changing the
** value of the cell.  This macro verifies that shallow copies are
** not misused.
*/
#ifdef SQLITE4_DEBUG
# define memAboutToChange(P,M) sqlite4VdbeMemAboutToChange(P,M)
#else
# define memAboutToChange(P,M)
#endif

/*
** The following global variable is incremented every time a cursor
** moves, either by the OP_SeekXX, OP_Next, or OP_Prev opcodes.  The test
** procedures use this information to make sure that indices are
** working correctly.  This variable has no function other than to
** help verify the correct operation of the library.
*/
#ifdef SQLITE4_TEST
int sqlite4_search_count = 0;
#endif

/*
** When this global variable is positive, it gets decremented once before
** each instruction in the VDBE.  When it reaches zero, the u1.isInterrupted
** field of the sqlite4 structure is set in order to simulate an interrupt.
**
** This facility is used for testing purposes only.  It does not function
** in an ordinary build.
*/
#ifdef SQLITE4_TEST
int sqlite4_interrupt_count = 0;
#endif

/*
** The next global variable is incremented each type the OP_Sort opcode
** is executed.  The test procedures use this information to make sure that
** sorting is occurring or not occurring at appropriate times.   This variable
** has no function other than to help verify the correct operation of the
** library.
*/
#ifdef SQLITE4_TEST
int sqlite4_sort_count = 0;
#endif

/*
** The next global variable records the size of the largest MEM_Blob
** or MEM_Str that has been used by a VDBE opcode.  The test procedures
** use this information to make sure that the zero-blob functionality
** is working correctly.   This variable has no function other than to
** help verify the correct operation of the library.
*/
#ifdef SQLITE4_TEST
int sqlite4_max_blobsize = 0;
static void updateMaxBlobsize(Mem *p){
  if( (p->flags & (MEM_Str|MEM_Blob))!=0 && p->n>sqlite4_max_blobsize ){
    sqlite4_max_blobsize = p->n;
  }
}
#endif

/*
** The next global variable is incremented each type the OP_Found opcode
** is executed. This is used to test whether or not the foreign key
** operation implemented using OP_FkIsZero is working. This variable
** has no function other than to help verify the correct operation of the
** library.
*/
#ifdef SQLITE4_TEST
int sqlite4_found_count = 0;
#endif

/*
** Test a register to see if it exceeds the current maximum blob size.
** If it does, record the new maximum blob size.
*/
#if defined(SQLITE4_TEST) && !defined(SQLITE4_OMIT_BUILTIN_TEST)
# define UPDATE_MAX_BLOBSIZE(P)  updateMaxBlobsize(P)
#else
# define UPDATE_MAX_BLOBSIZE(P)
#endif

/*
** Convert the given register into a string if it isn't one
** already. Return non-zero if a malloc() fails.
*/
#define Stringify(P, enc) \
   if(((P)->flags&(MEM_Str|MEM_Blob))==0 && sqlite4VdbeMemStringify(P,enc)) \
     { goto no_mem; }

/*
** An ephemeral string value (signified by the MEM_Ephem flag) contains
** a pointer to a dynamically allocated string where some other entity
** is responsible for deallocating that string.  Because the register
** does not control the string, it might be deleted without the register
** knowing it.
**
** This routine converts an ephemeral string into a dynamically allocated
** string that the register itself controls.  In other words, it
** converts an MEM_Ephem string into an MEM_Dyn string.
*/
#define Deephemeralize(P) \
   if( ((P)->flags&MEM_Ephem)!=0 \
       && sqlite4VdbeMemMakeWriteable(P) ){ goto no_mem;}

/*
** Argument pMem points at a register that will be passed to a
** user-defined function or returned to the user as the result of a query.
** This routine sets the pMem->type variable used by the sqlite4_value_*() 
** routines.
*/
void sqlite4VdbeMemStoreType(Mem *pMem){
  int flags = pMem->flags;
  if( flags & MEM_Null ){
    pMem->type = SQLITE4_NULL;
  }
  else if( flags & MEM_Int ){
    pMem->type = SQLITE4_INTEGER;
  }
  else if( flags & MEM_Real ){
    pMem->type = SQLITE4_FLOAT;
  }
  else if( flags & MEM_Str ){
    pMem->type = SQLITE4_TEXT;
  }else{
    pMem->type = SQLITE4_BLOB;
  }
}

/*
** Allocate VdbeCursor number iCur.  Return a pointer to it.  Return NULL
** if we run out of memory.
*/
static VdbeCursor *allocateCursor(
  Vdbe *p,              /* The virtual machine */
  int iCur,             /* Index of the new VdbeCursor */
  int nField,           /* Number of fields in the table or index */
  int iDb,              /* Database the cursor belongs to, or -1 */
  int isTrueCursor      /* True real cursor.  False for pseudo-table or vtab */
){
  /* Find the memory cell that will be used to store the blob of memory
  ** required for this VdbeCursor structure. It is convenient to use a 
  ** vdbe memory cell to manage the memory allocation required for a
  ** VdbeCursor structure for the following reasons:
  **
  **   * Sometimes cursor numbers are used for a couple of different
  **     purposes in a vdbe program. The different uses might require
  **     different sized allocations. Memory cells provide growable
  **     allocations.
  **
  ** Memory cells for cursors are allocated at the top of the address
  ** space. Memory cell (p->nMem) corresponds to cursor 0. Space for
  ** cursor 1 is managed by memory cell (p->nMem-1), etc.
  */
  Mem *pMem = &p->aMem[p->nMem-iCur];

  int nByte;
  VdbeCursor *pCx = 0;
  nByte = 
      ROUND8(sizeof(VdbeCursor)) + 
      2*nField*sizeof(u32);

  assert( iCur<p->nCursor );
  if( p->apCsr[iCur] ){
    sqlite4VdbeFreeCursor(p->apCsr[iCur]);
    p->apCsr[iCur] = 0;
  }
  if( SQLITE4_OK==sqlite4VdbeMemGrow(pMem, nByte, 0) ){
    p->apCsr[iCur] = pCx = (VdbeCursor*)pMem->z;
    memset(pCx, 0, sizeof(VdbeCursor));
    pCx->db = p->db;
    pCx->iDb = iDb;
    pCx->nField = nField;
    pCx->rowChnged = 1;
    sqlite4_buffer_init(&pCx->sSeekKey, p->db->pEnv->pMM);
  }
  return pCx;
}

/*
** Try to convert a value into a numeric representation if we can
** do so without loss of information.  In other words, if the string
** looks like a number, convert it into a number.  If it does not
** look like a number, leave it alone.
*/
static void applyNumericAffinity(Mem *pRec){
  if( (pRec->flags & (MEM_Real|MEM_Int))==0 && (pRec->flags & MEM_Str) ){
    int flags = pRec->enc | SQLITE4_IGNORE_WHITESPACE;
    int bReal = 0;
    sqlite4_num num;
    
    num = sqlite4_num_from_text(pRec->z, pRec->n, flags, &bReal);
    if( sqlite4_num_isnan(num)==0 ){
      pRec->u.num = num;
      MemSetTypeFlag(pRec, (bReal ? MEM_Real : MEM_Int));
    }
  }
}

/*
** Processing is determine by the affinity parameter:
**
** SQLITE4_AFF_INTEGER:
** SQLITE4_AFF_REAL:
** SQLITE4_AFF_NUMERIC:
**    Try to convert pRec to an integer representation or a 
**    floating-point representation if an integer representation
**    is not possible.  Note that the integer representation is
**    always preferred, even if the affinity is REAL, because
**    an integer representation is more space efficient on disk.
**
** SQLITE4_AFF_TEXT:
**    Convert pRec to a text representation.
**
** SQLITE4_AFF_NONE:
**    No-op.  pRec is unchanged.
*/
static void applyAffinity(
  Mem *pRec,          /* The value to apply affinity to */
  char affinity,      /* The affinity to be applied */
  u8 enc              /* Use this text encoding */
){
  if( affinity==SQLITE4_AFF_TEXT ){
    /* Only attempt the conversion to TEXT if there is an integer or real
    ** representation (blob and NULL do not get converted) but no string
    ** representation.
    */
    if( 0==(pRec->flags&MEM_Str) && (pRec->flags&(MEM_Real|MEM_Int)) ){
      sqlite4VdbeMemStringify(pRec, enc);
    }
    pRec->flags &= ~(MEM_Real|MEM_Int);
  }else if( affinity!=SQLITE4_AFF_NONE ){
    assert( affinity==SQLITE4_AFF_INTEGER || affinity==SQLITE4_AFF_REAL
             || affinity==SQLITE4_AFF_NUMERIC );
    applyNumericAffinity(pRec);
    if( pRec->flags & MEM_Real ){
      sqlite4VdbeIntegerAffinity(pRec);
    }
  }
}

/*
** Try to convert the type of a function argument or a result column
** into a numeric representation.  Use either INTEGER or REAL whichever
** is appropriate.  But only do the conversion if it is possible without
** loss of information and return the revised type of the argument.
*/
int sqlite4_value_numeric_type(sqlite4_value *pVal){
  Mem *pMem = (Mem*)pVal;
  if( pMem->type==SQLITE4_TEXT ){
    applyNumericAffinity(pMem);
    sqlite4VdbeMemStoreType(pMem);
  }
  return pMem->type;
}

sqlite4_num sqlite4_value_num(sqlite4_value *pVal){
  return sqlite4VdbeNumValue((Mem*)pVal);
}

/*
** Exported version of applyAffinity(). This one works on sqlite4_value*, 
** not the internal Mem* type.
*/
void sqlite4ValueApplyAffinity(
  sqlite4_value *pVal, 
  u8 affinity, 
  u8 enc
){
  applyAffinity((Mem *)pVal, affinity, enc);
}

#ifdef SQLITE4_DEBUG
/*
** Write a nice string representation of the contents of cell pMem
** into buffer zBuf, length nBuf.
*/
void sqlite4VdbeMemPrettyPrint(Mem *pMem, char *zBuf){
  char *zCsr = zBuf;
  int f = pMem->flags;

  static const char *const encnames[] = {"(X)", "(8)", "(16LE)", "(16BE)"};

  if( f&MEM_Blob ){
    int i;
    char c;
    if( f & MEM_Dyn ){
      c = 'z';
      assert( (f & (MEM_Static|MEM_Ephem))==0 );
    }else if( f & MEM_Static ){
      c = 't';
      assert( (f & (MEM_Dyn|MEM_Ephem))==0 );
    }else if( f & MEM_Ephem ){
      c = 'e';
      assert( (f & (MEM_Static|MEM_Dyn))==0 );
    }else{
      c = 's';
    }

    zCsr += sqlite4_snprintf(zCsr, 100, "%c", c);
    zCsr += sqlite4_snprintf(zCsr, 100, "%d[", pMem->n);
    for(i=0; i<16 && i<pMem->n; i++){
      zCsr += sqlite4_snprintf(zCsr, 100, "%02X", ((int)pMem->z[i] & 0xFF));
    }
    for(i=0; i<16 && i<pMem->n; i++){
      char z = pMem->z[i];
      if( z<32 || z>126 ) *zCsr++ = '.';
      else *zCsr++ = z;
    }

    zCsr += sqlite4_snprintf(zCsr, 100, "]%s", encnames[pMem->enc]);
    *zCsr = '\0';
  }else if( f & MEM_Str ){
    int j, k;
    zBuf[0] = ' ';
    if( f & MEM_Dyn ){
      zBuf[1] = 'z';
      assert( (f & (MEM_Static|MEM_Ephem))==0 );
    }else if( f & MEM_Static ){
      zBuf[1] = 't';
      assert( (f & (MEM_Dyn|MEM_Ephem))==0 );
    }else if( f & MEM_Ephem ){
      zBuf[1] = 'e';
      assert( (f & (MEM_Static|MEM_Dyn))==0 );
    }else{
      zBuf[1] = 's';
    }
    k = 2;
    k += sqlite4_snprintf(&zBuf[k], 100, "%d", pMem->n);
    zBuf[k++] = '[';
    for(j=0; j<15 && j<pMem->n; j++){
      u8 c = pMem->z[j];
      if( c>=0x20 && c<0x7f ){
        zBuf[k++] = c;
      }else{
        zBuf[k++] = '.';
      }
    }
    zBuf[k++] = ']';
    k += sqlite4_snprintf(&zBuf[k], 100, encnames[pMem->enc]);
    zBuf[k++] = 0;
  }
}
#endif

#ifdef SQLITE4_DEBUG
/*
** Print the value of a register for tracing purposes:
*/
static void memTracePrint(FILE *out, Mem *p){
  if( p->flags & MEM_Null ){
    fprintf(out, " NULL");
  }else if( p->flags & (MEM_Int|MEM_Real) ){
    char aNum[31];
    char *zFlags = "r";
    sqlite4_num_to_text(p->u.num, aNum, (p->flags & MEM_Real));
    if( (p->flags & (MEM_Int|MEM_Str))==(MEM_Int|MEM_Str) ){
      zFlags = "si";
    }else if( p->flags & MEM_Int ){
      zFlags = "i";
    }
    fprintf(out, " %s:%s", zFlags, aNum);
  }else if( p->flags & MEM_RowSet ){
    fprintf(out, " (keyset)");
  }else{
    char zBuf[200];
    sqlite4VdbeMemPrettyPrint(p, zBuf);
    fprintf(out, " ");
    fprintf(out, "%s", zBuf);
  }
}
static void registerTrace(FILE *out, int iReg, Mem *p){
  fprintf(out, "REG[%d] = ", iReg);
  memTracePrint(out, p);
  fprintf(out, "\n");
}
#endif

#ifdef SQLITE4_DEBUG
static int assertFlagsOk(Mem *p){
  u16 flags = p->flags;
  assert( (flags&MEM_Int)==0 || (flags&MEM_Real)==0 );
  return 1;
}
#endif

#ifdef SQLITE4_DEBUG
# define REGISTER_TRACE(R,M) \
    if(assertFlagsOk(M) && p->trace)registerTrace(p->trace,R,M)
#else
# define REGISTER_TRACE(R,M)
#endif


#ifdef VDBE_PROFILE

/* 
** hwtime.h contains inline assembler code for implementing 
** high-performance timing routines.
*/
#include "hwtime.h"

#endif

/*
** The CHECK_FOR_INTERRUPT macro defined here looks to see if the
** sqlite4_interrupt() routine has been called.  If it has been, then
** processing of the VDBE program is interrupted.
**
** This macro added to every instruction that does a jump in order to
** implement a loop.  This test used to be on every single instruction,
** but that meant we more testing than we needed.  By only testing the
** flag on jump instructions, we get a (small) speed improvement.
*/
#define CHECK_FOR_INTERRUPT \
   if( db->u1.isInterrupted ) goto abort_due_to_interrupt;

/*
** Transfer error message text from an sqlite4_vtab.zErrMsg (text stored
** in memory obtained from sqlite4_malloc) into a Vdbe.zErrMsg (text stored
** in memory obtained from sqlite4DbMalloc).
*/
/*UNUSED static*/ void importVtabErrMsg(Vdbe *p, sqlite4_vtab *pVtab){
  sqlite4 *db = p->db;
  sqlite4DbFree(db, p->zErrMsg);
  p->zErrMsg = sqlite4DbStrDup(db, pVtab->zErrMsg);
  sqlite4_free(db->pEnv, pVtab->zErrMsg);
  pVtab->zErrMsg = 0;
}

/*
** Return a pointer to a register in the root frame.
*/
static Mem *sqlite4RegisterInRootFrame(Vdbe *p, int i){
  if( p->pFrame ){
    VdbeFrame *pFrame;
    for(pFrame=p->pFrame; pFrame->pParent; pFrame=pFrame->pParent);
    return &pFrame->aMem[i];
  }else{
    return &p->aMem[i];
  }
}

/*
** Execute as much of a VDBE program as we can then return.
**
** sqlite4VdbeMakeReady() must be called before this routine in order to
** close the program with a final OP_Halt and to set up the callbacks
** and the error message pointer.
**
** Whenever a row or result data is available, this routine will either
** invoke the result callback (if there is one) or return with
** SQLITE4_ROW.
**
** If an attempt is made to open a locked database, then this routine
** will either invoke the busy callback (if there is one) or it will
** return SQLITE4_BUSY.
**
** If an error occurs, an error message is written to memory obtained
** from sqlite4_malloc() and p->zErrMsg is made to point to that memory.
** The error code is stored in p->rc and this routine returns SQLITE4_ERROR.
**
** If the callback ever returns non-zero, then the program exits
** immediately.  There will be no error message but the p->rc field is
** set to SQLITE4_ABORT and this routine will return SQLITE4_ERROR.
**
** A memory allocation error causes p->rc to be set to SQLITE4_NOMEM and this
** routine to return SQLITE4_ERROR.
**
** Other fatal errors return SQLITE4_ERROR.
**
** After this routine has finished, sqlite4VdbeFinalize() should be
** used to clean up the mess that was left behind.
*/
int sqlite4VdbeExec(
  Vdbe *p                    /* The VDBE */
){
  int pc=0;                  /* The program counter */
  Op *aOp = p->aOp;          /* Copy of p->aOp */
  Op *pOp;                   /* Current operation */
  int rc = SQLITE4_OK;        /* Value to return */
  sqlite4 *db = p->db;       /* The database */
  u8 resetSchemaOnFault = 0; /* Reset schema after an error if positive */
  u8 encoding = ENC(db);     /* The database encoding */
#ifndef SQLITE4_OMIT_PROGRESS_CALLBACK
  int checkProgress;         /* True if progress callbacks are enabled */
  int nProgressOps = 0;      /* Opcodes executed since progress callback. */
#endif
  Mem *aMem = p->aMem;       /* Copy of p->aMem */
  Mem *pIn1 = 0;             /* 1st input operand */
  Mem *pIn2 = 0;             /* 2nd input operand */
  Mem *pIn3 = 0;             /* 3rd input operand */
  Mem *pOut = 0;             /* Output operand */
  int iCompare = 0;          /* Result of last OP_Compare operation */
  int *aPermute = 0;         /* Permutation of columns for OP_Compare */
#ifdef VDBE_PROFILE
  u64 start;                 /* CPU clock count at start of opcode */
  int origPc;                /* Program counter at start of opcode */
#endif
  /*** INSERT STACK UNION HERE ***/

  assert( p->magic==VDBE_MAGIC_RUN );  /* sqlite4_step() verifies this */
  if( p->rc==SQLITE4_NOMEM ){
    /* This happens if a malloc() inside a call to sqlite4_column_text() or
    ** sqlite4_column_text16() failed.  */
    goto no_mem;
  }
  assert( p->rc==SQLITE4_OK || p->rc==SQLITE4_BUSY );
  p->rc = SQLITE4_OK;
  assert( p->explain==0 );
  p->pResultSet = 0;
  CHECK_FOR_INTERRUPT;
  sqlite4VdbeIOTraceSql(p);
#ifndef SQLITE4_OMIT_PROGRESS_CALLBACK
  checkProgress = db->xProgress!=0;
#endif
#ifdef SQLITE4_DEBUG
  sqlite4BeginBenignMalloc(db->pEnv);
  if( p->pc==0  && (db->flags & SQLITE4_VdbeListing)!=0 ){
    int i;
    printf("VDBE Program Listing:\n");
    sqlite4VdbePrintSql(p);
    for(i=0; i<p->nOp; i++){
      sqlite4VdbePrintOp(stdout, i, &aOp[i]);
    }
  }
  sqlite4EndBenignMalloc(db->pEnv);
#endif
  for(pc=p->pc; rc==SQLITE4_OK; pc++){
    assert( pc>=0 && pc<p->nOp );
    if( db->mallocFailed ) goto no_mem;
#ifdef VDBE_PROFILE
    origPc = pc;
    start = sqlite4Hwtime();
#endif
    pOp = &aOp[pc];

    /* Only allow tracing if SQLITE4_DEBUG is defined.
    */
#ifdef SQLITE4_DEBUG
    if( p->trace ){
      if( pc==0 ){
        printf("VDBE Execution Trace:\n");
        sqlite4VdbePrintSql(p);
      }
      sqlite4VdbePrintOp(p->trace, pc, pOp);
    }
#endif
      

    /* Check to see if we need to simulate an interrupt.  This only happens
    ** if we have a special test build.
    */
#ifdef SQLITE4_TEST
    if( sqlite4_interrupt_count>0 ){
      sqlite4_interrupt_count--;
      if( sqlite4_interrupt_count==0 ){
        sqlite4_interrupt(db);
      }
    }
#endif

#ifndef SQLITE4_OMIT_PROGRESS_CALLBACK
    /* Call the progress callback if it is configured and the required number
    ** of VDBE ops have been executed (either since this invocation of
    ** sqlite4VdbeExec() or since last time the progress callback was called).
    ** If the progress callback returns non-zero, exit the virtual machine with
    ** a return code SQLITE4_ABORT.
    */
    if( checkProgress ){
      if( db->nProgressOps==nProgressOps ){
        int prc;
        prc = db->xProgress(db->pProgressArg);
        if( prc!=0 ){
          rc = SQLITE4_INTERRUPT;
          goto vdbe_error_halt;
        }
        nProgressOps = 0;
      }
      nProgressOps++;
    }
#endif

    /* On any opcode with the "out2-prerelase" tag, free any
    ** external allocations out of mem[p2] and set mem[p2] to be
    ** an undefined integer.  Opcodes will either fill in the integer
    ** value or convert mem[p2] to a different type.
    */
    assert( pOp->opflags==sqlite4OpcodeProperty[pOp->opcode] );
    if( pOp->opflags & OPFLG_OUT2_PRERELEASE ){
      assert( pOp->p2>0 );
      assert( pOp->p2<=p->nMem );
      pOut = &aMem[pOp->p2];
      memAboutToChange(p, pOut);
      VdbeMemRelease(pOut);
      pOut->flags = MEM_Int;
    }

    /* Sanity checking on other operands */
#ifdef SQLITE4_DEBUG
    if( (pOp->opflags & OPFLG_IN1)!=0 ){
      assert( pOp->p1>0 );
      assert( pOp->p1<=p->nMem );
      assert( memIsValid(&aMem[pOp->p1]) );
      REGISTER_TRACE(pOp->p1, &aMem[pOp->p1]);
    }
    if( (pOp->opflags & OPFLG_IN2)!=0 ){
      assert( pOp->p2>0 );
      assert( pOp->p2<=p->nMem );
      assert( memIsValid(&aMem[pOp->p2]) );
      REGISTER_TRACE(pOp->p2, &aMem[pOp->p2]);
    }
    if( (pOp->opflags & OPFLG_IN3)!=0 ){
      assert( pOp->p3>0 );
      assert( pOp->p3<=p->nMem );
      assert( memIsValid(&aMem[pOp->p3]) );
      REGISTER_TRACE(pOp->p3, &aMem[pOp->p3]);
    }
    if( (pOp->opflags & OPFLG_OUT2)!=0 ){
      assert( pOp->p2>0 );
      assert( pOp->p2<=p->nMem );
      memAboutToChange(p, &aMem[pOp->p2]);
    }
    if( (pOp->opflags & OPFLG_OUT3)!=0 ){
      assert( pOp->p3>0 );
      assert( pOp->p3<=p->nMem );
      memAboutToChange(p, &aMem[pOp->p3]);
    }
#endif
    printf("\n/**  VDBE OPCODE: %d\n", pOp->opcode);
    printf("/**\t#p1: %d   #p2: %d   #p3: %d   #p4type: %d   #p5: %d\n",pOp->p1, pOp->p2, pOp->p3, pOp->p4type, pOp->p5);

            
    switch( pOp->opcode ){

/*****************************************************************************
** What follows is a massive switch statement where each case implements a
** separate instruction in the virtual machine.  If we follow the usual
** indentation conventions, each case should be indented by 6 spaces.  But
** that is a lot of wasted space on the left margin.  So the code within
** the switch statement will break with convention and be flush-left. Another
** big comment (similar to this one) will mark the point in the code where
** we transition back to normal indentation.
**
** The formatting of each case is important.  The makefile for SQLite
** generates two C files "opcodes.h" and "opcodes.c" by scanning this
** file looking for lines that begin with "case OP_".  The opcodes.h files
** will be filled with #defines that give unique integer values to each
** opcode and the opcodes.c file is filled with an array of strings where
** each string is the symbolic name for the corresponding opcode.  If the
** case statement is followed by a comment of the form "/# same as ... #/"
** that comment is used to determine the particular value of the opcode.
**
** Other keywords in the comment that follows each case are used to
** construct the OPFLG_INITIALIZER value that initializes opcodeProperty[].
** Keywords include: in1, in2, in3, out2_prerelease, out2, out3.  See
** the mkopcodeh.awk script for additional information.
**
** Documentation about VDBE opcodes is generated by scanning this file
** for lines of that contain "Opcode:".  That line and all subsequent
** comment lines are used in the generation of the opcode.html documentation
** file.
**
** SUMMARY:
**
**     Formatting is important to scripts that scan this file.
**     Do not deviate from the formatting style currently in use.
**
*****************************************************************************/

/* Opcode:  Goto * P2 * * *
**
** An unconditional jump to address P2.
** The next instruction executed will be 
** the one at index P2 from the beginning of
** the program.
*/
case OP_Goto: {             /* jump */
  CHECK_FOR_INTERRUPT;
  pc = pOp->p2 - 1;
  break;
}

/* Opcode:  Gosub P1 P2 * * *
**
** Write the current address onto register P1
** and then jump to address P2.
*/
case OP_Gosub: {            /* jump */
  assert( pOp->p1>0 && pOp->p1<=p->nMem );
  pIn1 = &aMem[pOp->p1];
  assert( (pIn1->flags & MEM_Dyn)==0 );
  memAboutToChange(p, pIn1);
  pIn1->flags = MEM_Int;
  pIn1->u.num = sqlite4_num_from_int64((i64)pc);
  REGISTER_TRACE(pOp->p1, pIn1);
  pc = pOp->p2 - 1;
  break;
}

/* Opcode:  Return P1 * * * *
**
** Jump to the next instruction after the address in register P1.
*/
case OP_Return: {           /* in1 */
  pIn1 = &aMem[pOp->p1];
  assert( pIn1->flags & MEM_Int );
  pc = sqlite4_num_to_int32(pIn1->u.num, 0);
  break;
}

/* Opcode:  Yield P1 * * * *
**
** Swap the program counter with the value in register P1.
*/
case OP_Yield: {            /* in1 */
  int pcDest;
  pIn1 = &aMem[pOp->p1];
  assert( (pIn1->flags & MEM_Dyn)==0 );
  pIn1->flags = MEM_Int;
  pcDest = sqlite4_num_to_int32(pIn1->u.num, 0);
  pIn1->u.num = sqlite4_num_from_int64(pc);
  REGISTER_TRACE(pOp->p1, pIn1);
  pc = pcDest;
  break;
}

/* Opcode:  HaltIfNull  P1 P2 P3 P4 *
**
** Check the value in register P3.  If it is NULL then Halt using
** parameter P1, P2, and P4 as if this were a Halt instruction.  If the
** value in register P3 is not NULL, then this routine is a no-op.
*/
case OP_HaltIfNull: {      /* in3 */
  pIn3 = &aMem[pOp->p3];
  if( (pIn3->flags & MEM_Null)==0 ) break;
  /* Fall through into OP_Halt */
}

/* Opcode:  Halt P1 P2 * P4 *
**
** Exit immediately.  All open cursors, etc are closed
** automatically.
**
** P1 is the result code returned by sqlite4_exec(), sqlite4_reset(),
** or sqlite4_finalize().  For a normal halt, this should be SQLITE4_OK (0).
** For errors, it can be some other value.  If P1!=0 then P2 will determine
** whether or not to rollback the current transaction.  Do not rollback
** if P2==OE_Fail. Do the rollback if P2==OE_Rollback.  If P2==OE_Abort,
** then back out all changes that have occurred during this execution of the
** VDBE, but do not rollback the transaction. 
**
** If P4 is not null then it is an error message string.
**
** There is an implied "Halt 0 0 0" instruction inserted at the very end of
** every program.  So a jump past the last instruction of the program
** is the same as executing Halt.
*/
case OP_Halt: {
  if( pOp->p1==SQLITE4_OK && p->pFrame ){
    /* Halt the sub-program. Return control to the parent frame. */
    VdbeFrame *pFrame = p->pFrame;
    p->pFrame = pFrame->pParent;
    p->nFrame--;
    sqlite4VdbeSetChanges(db, p->nChange);
    pc = sqlite4VdbeFrameRestore(pFrame);
    if( pOp->p2==OE_Ignore ){
      /* Instruction pc is the OP_Program that invoked the sub-program 
      ** currently being halted. If the p2 instruction of this OP_Halt
      ** instruction is set to OE_Ignore, then the sub-program is throwing
      ** an IGNORE exception. In this case jump to the address specified
      ** as the p2 of the calling OP_Program.  */
      pc = p->aOp[pc].p2-1;
    }
    aOp = p->aOp;
    aMem = p->aMem;
    break;
  }

  p->rc = pOp->p1;
  p->errorAction = (u8)pOp->p2;
  p->pc = pc;
  if( pOp->p4.z ){
    assert( p->rc!=SQLITE4_OK );
    sqlite4SetString(&p->zErrMsg, db, "%s", pOp->p4.z);
    testcase( sqlite4DefaultEnv.xLog!=0 );
    sqlite4_log(db->pEnv, pOp->p1,
                "abort at %d in [%s]: %s", pc, p->zSql, pOp->p4.z);
  }else if( p->rc ){
    testcase( sqlite4DefaultEnv.xLog!=0 );
    sqlite4_log(db->pEnv, pOp->p1,
                "constraint failed at %d in [%s]", pc, p->zSql);
  }
  rc = sqlite4VdbeHalt(p);
  assert( rc==SQLITE4_BUSY || rc==SQLITE4_OK || rc==SQLITE4_ERROR );
  if( rc==SQLITE4_BUSY ){
    p->rc = rc = SQLITE4_BUSY;
  }else{
    assert( rc==SQLITE4_OK || p->rc==SQLITE4_CONSTRAINT );
    assert( rc==SQLITE4_OK || db->nDeferredCons>0 );
    rc = p->rc ? SQLITE4_ERROR : SQLITE4_DONE;
  }
  goto vdbe_return;
}

/* Opcode: Integer P1 P2 * * *
**
** The 32-bit integer value P1 is written into register P2.
*/
case OP_Integer: {         /* out2-prerelease */
  pOut->u.num = sqlite4_num_from_int64((i64)pOp->p1);
  MemSetTypeFlag(pOut, MEM_Int);
  break;
}

/* Opcode: Num P1 P2 * P4 *
**
** P4 is a pointer to an sqlite4_num value. Write that value into 
** register P2. Set the register flags to MEM_Int if P1 is non-zero,
** or MEM_Real otherwise.
*/
case OP_Num: {            /* out2-prerelease */
  pOut->flags = (pOp->p1 ? MEM_Int : MEM_Real);
  pOut->u.num = *(pOp->p4.pNum);
  break;
}

/* Opcode: String8 * P2 * P4 *
**
** P4 points to a nul terminated UTF-8 string. This opcode is transformed 
** into an OP_String before it is executed for the first time.
*/
case OP_String8: {         /* same as TK_STRING, out2-prerelease */
  assert( pOp->p4.z!=0 );
  pOp->opcode = OP_String;
  pOp->p1 = sqlite4Strlen30(pOp->p4.z);

#ifndef SQLITE4_OMIT_UTF16
  if( encoding!=SQLITE4_UTF8 ){
    rc = sqlite4VdbeMemSetStr(pOut, pOp->p4.z, -1, SQLITE4_UTF8,
                              SQLITE4_STATIC, 0);
    if( rc==SQLITE4_TOOBIG ) goto too_big;
    if( SQLITE4_OK!=sqlite4VdbeChangeEncoding(pOut, encoding) ) goto no_mem;
    assert( pOut->zMalloc==pOut->z );
    assert( pOut->flags & MEM_Dyn );
    pOut->zMalloc = 0;
    pOut->flags |= MEM_Static;
    pOut->flags &= ~MEM_Dyn;
    if( pOp->p4type==P4_DYNAMIC ){
      sqlite4DbFree(db, pOp->p4.z);
    }
    pOp->p4type = P4_DYNAMIC;
    pOp->p4.z = pOut->z;
    pOp->p1 = pOut->n;
  }
#endif
  if( pOp->p1>db->aLimit[SQLITE4_LIMIT_LENGTH] ){
    goto too_big;
  }
  /* Fall through to the next case, OP_String */
}
  
/* Opcode: String P1 P2 * P4 *
**
** The string value P4 of length P1 (bytes) is stored in register P2.
*/
case OP_String: {          /* out2-prerelease */
  assert( pOp->p4.z!=0 );
  pOut->flags = MEM_Str|MEM_Static|MEM_Term;
  pOut->z = pOp->p4.z;
  pOut->n = pOp->p1;
  pOut->enc = encoding;
  UPDATE_MAX_BLOBSIZE(pOut);
  break;
}

/* Opcode: Null * P2 P3 * *
**
** Write a NULL into registers P2.  If P3 greater than P2, then also write
** NULL into register P3 and ever register in between P2 and P3.  If P3
** is less than P2 (typically P3 is zero) then only register P2 is
** set to NULL
*/
case OP_Null: {           /* out2-prerelease */
  int cnt;
  cnt = pOp->p3-pOp->p2;
  assert( pOp->p3<=p->nMem );
  pOut->flags = MEM_Null;
  while( cnt>0 ){
    pOut++;
    memAboutToChange(p, pOut);
    VdbeMemRelease(pOut);
    pOut->flags = MEM_Null;
    cnt--;
  }
  break;
}


/* Opcode: Blob P1 P2 * P4
**
** P4 points to a blob of data P1 bytes long.  Store this
** blob in register P2.
*/
case OP_Blob: {                /* out2-prerelease */
  assert( pOp->p1 <= SQLITE4_MAX_LENGTH );
  sqlite4VdbeMemSetStr(pOut, pOp->p4.z, pOp->p1, 0, 0, 0);
  pOut->enc = encoding;
  UPDATE_MAX_BLOBSIZE(pOut);
  break;
}

/* Opcode: Variable P1 P2 * P4 *
**
** Transfer the values of bound parameter P1 into register P2
**
** If the parameter is named, then its name appears in P4 and P3==1.
** The P4 value is used by sqlite4_bind_parameter_name().
*/
case OP_Variable: {            /* out2-prerelease */
  Mem *pVar;       /* Value being transferred */

  assert( pOp->p1>0 && pOp->p1<=p->nVar );
  assert( pOp->p4.z==0 || pOp->p4.z==p->azVar[pOp->p1-1] );
  pVar = &p->aVar[pOp->p1 - 1];
  if( sqlite4VdbeMemTooBig(pVar) ){
    goto too_big;
  }
  sqlite4VdbeMemShallowCopy(pOut, pVar, MEM_Static);
  UPDATE_MAX_BLOBSIZE(pOut);
  break;
}

/* Opcode: Move P1 P2 P3 * *
**
** Move the values in register P1..P1+P3-1 over into
** registers P2..P2+P3-1.  Registers P1..P1+P1-1 are
** left holding a NULL.  It is an error for register ranges
** P1..P1+P3-1 and P2..P2+P3-1 to overlap.
*/
case OP_Move: {
  char *zMalloc;   /* Holding variable for allocated memory */
  int n;           /* Number of registers left to copy */
  int p1;          /* Register to copy from */
  int p2;          /* Register to copy to */

  n = pOp->p3;
  p1 = pOp->p1;
  p2 = pOp->p2;
  assert( n>0 && p1>0 && p2>0 );
  assert( p1+n<=p2 || p2+n<=p1 );

  pIn1 = &aMem[p1];
  pOut = &aMem[p2];
  while( n-- ){
    assert( pOut<=&aMem[p->nMem] );
    assert( pIn1<=&aMem[p->nMem] );
    assert( memIsValid(pIn1) );
    memAboutToChange(p, pOut);
    zMalloc = pOut->zMalloc;
    pOut->zMalloc = 0;
    sqlite4VdbeMemMove(pOut, pIn1);
#ifdef SQLITE4_DEBUG
    if( pOut->pScopyFrom>=&aMem[p1] && pOut->pScopyFrom<&aMem[p1+pOp->p3] ){
      pOut->pScopyFrom += p1 - pOp->p2;
    }
#endif
    pIn1->zMalloc = zMalloc;
    REGISTER_TRACE(p2++, pOut);
    pIn1++;
    pOut++;
  }
  break;
}

/* Opcode: Copy P1 P2 * * *
**
** Make a copy of register P1 into register P2.
**
** This instruction makes a deep copy of the value.  A duplicate
** is made of any string or blob constant.  See also OP_SCopy.
*/
case OP_Copy: {             /* in1, out2 */
  pIn1 = &aMem[pOp->p1];
  pOut = &aMem[pOp->p2];
  assert( pOut!=pIn1 );
  sqlite4VdbeMemShallowCopy(pOut, pIn1, MEM_Ephem);
  Deephemeralize(pOut);
  REGISTER_TRACE(pOp->p2, pOut);
  break;
}

/* Opcode: SCopy P1 P2 * * *
**
** Make a shallow copy of register P1 into register P2.
**
** This instruction makes a shallow copy of the value.  If the value
** is a string or blob, then the copy is only a pointer to the
** original and hence if the original changes so will the copy.
** Worse, if the original is deallocated, the copy becomes invalid.
** Thus the program must guarantee that the original will not change
** during the lifetime of the copy.  Use OP_Copy to make a complete
** copy.
*/
case OP_SCopy: {            /* in1, out2 */
  pIn1 = &aMem[pOp->p1];
  pOut = &aMem[pOp->p2];
  assert( pOut!=pIn1 );
  sqlite4VdbeMemShallowCopy(pOut, pIn1, MEM_Ephem);
#ifdef SQLITE4_DEBUG
  if( pOut->pScopyFrom==0 ) pOut->pScopyFrom = pIn1;
#endif
  REGISTER_TRACE(pOp->p2, pOut);
  break;
}

/* Opcode: ResultRow P1 P2 * * *
**
** The registers P1 through P1+P2-1 contain a single row of
** results. This opcode causes the sqlite4_step() call to terminate
** with an SQLITE4_ROW return code and it sets up the sqlite4_stmt
** structure to provide access to the top P1 values as the result
** row.
*/
case OP_ResultRow: {
  Mem *pMem;
  int i;
  assert( p->nResColumn==pOp->p2 );
  assert( pOp->p1>0 );
  assert( pOp->p1+pOp->p2<=p->nMem+1 );
  assert( p->nFkConstraint==0 );

  /* If the SQLITE4_CountRows flag is set in sqlite4.flags mask, then 
  ** DML statements invoke this opcode to return the number of rows 
  ** modified to the user. This is the only way that a VM that
  ** opens a statement transaction may invoke this opcode.
  **
  ** In case this is such a statement, close any statement transaction
  ** opened by this VM before returning control to the user. This is to
  ** ensure that statement-transactions are always nested, not overlapping.
  ** If the open statement-transaction is not closed here, then the user
  ** may step another VM that opens its own statement transaction. This
  ** may lead to overlapping statement transactions.
  **
  ** The statement transaction is never a top-level transaction.  Hence
  ** the RELEASE call below can never fail.
  */
  rc = sqlite4VdbeCloseStatement(p, SAVEPOINT_RELEASE);
  if( NEVER(rc!=SQLITE4_OK) ){
    break;
  }

  /* Invalidate all ephemeral cursor row caches */
  p->cacheCtr = (p->cacheCtr + 2)|1;

  /* Make sure the results of the current row are \000 terminated
  ** and have an assigned type.  The results are de-ephemeralized as
  ** a side effect.
  */
  pMem = p->pResultSet = &aMem[pOp->p1];
  for(i=0; i<pOp->p2; i++){
    assert( memIsValid(&pMem[i]) );
    Deephemeralize(&pMem[i]);
    assert( (pMem[i].flags & MEM_Ephem)==0
            || (pMem[i].flags & (MEM_Str|MEM_Blob))==0 );
    sqlite4VdbeMemNulTerminate(&pMem[i]);
    sqlite4VdbeMemStoreType(&pMem[i]);
    REGISTER_TRACE(pOp->p1+i, &pMem[i]);
  }
  if( db->mallocFailed ) goto no_mem;

  /* Return SQLITE4_ROW
  */
  p->pc = pc + 1;
  rc = SQLITE4_ROW;
  goto vdbe_return;
}

/* Opcode: Concat P1 P2 P3 * *
**
** Add the text in register P1 onto the end of the text in
** register P2 and store the result in register P3.
** If either the P1 or P2 text are NULL then store NULL in P3.
**
**   P3 = P2 || P1
**
** It is illegal for P1 and P3 to be the same register. Sometimes,
** if P3 is the same register as P2, the implementation is able
** to avoid a memcpy().
*/
case OP_Concat: {           /* same as TK_CONCAT, in1, in2, out3 */
  i64 nByte;

  pIn1 = &aMem[pOp->p1];
  pIn2 = &aMem[pOp->p2];
  pOut = &aMem[pOp->p3];
  assert( pIn1!=pOut );
  if( (pIn1->flags | pIn2->flags) & MEM_Null ){
    sqlite4VdbeMemSetNull(pOut);
    break;
  }
  Stringify(pIn1, encoding);
  Stringify(pIn2, encoding);
  nByte = pIn1->n + pIn2->n;
  if( nByte>db->aLimit[SQLITE4_LIMIT_LENGTH] ){
    goto too_big;
  }
  MemSetTypeFlag(pOut, MEM_Str);
  if( sqlite4VdbeMemGrow(pOut, (int)nByte+2, pOut==pIn2) ){
    goto no_mem;
  }
  if( pOut!=pIn2 ){
    memcpy(pOut->z, pIn2->z, pIn2->n);
  }
  memcpy(&pOut->z[pIn2->n], pIn1->z, pIn1->n);
  pOut->z[nByte] = 0;
  pOut->z[nByte+1] = 0;
  pOut->flags |= MEM_Term;
  pOut->n = (int)nByte;
  pOut->enc = encoding;
  UPDATE_MAX_BLOBSIZE(pOut);
  break;
}

/* Opcode: Add P1 P2 P3 * *
**
** Add the value in register P1 to the value in register P2
** and store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: Multiply P1 P2 P3 * *
**
**
** Multiply the value in register P1 by the value in register P2
** and store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: Subtract P1 P2 P3 * *
**
** Subtract the value in register P1 from the value in register P2
** and store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: Divide P1 P2 P3 * *
**
** Divide the value in register P1 by the value in register P2
** and store the result in register P3 (P3=P2/P1). If the value in 
** register P1 is zero, then the result is NULL. If either input is 
** NULL, the result is NULL.
*/
/* Opcode: Remainder P1 P2 P3 * *
**
** Compute the remainder after integer division of the value in
** register P1 by the value in register P2 and store the result in P3. 
** If the value in register P2 is zero the result is NULL.
** If either operand is NULL, the result is NULL.
*/
case OP_Add:                   /* same as TK_PLUS, in1, in2, out3 */
case OP_Subtract:              /* same as TK_MINUS, in1, in2, out3 */
case OP_Multiply:              /* same as TK_STAR, in1, in2, out3 */
case OP_Divide:                /* same as TK_SLASH, in1, in2, out3 */
case OP_Remainder: {           /* same as TK_REM, in1, in2, out3 */
  int flags;      /* Combined MEM_* flags from both inputs */
  i64 iA;         /* Integer value of left operand */
  i64 iB;         /* Integer value of right operand */
  sqlite4_num num1;
  sqlite4_num num2;

  pIn1 = &aMem[pOp->p1];
  applyNumericAffinity(pIn1);
  pIn2 = &aMem[pOp->p2];
  applyNumericAffinity(pIn2);
  pOut = &aMem[pOp->p3];
  flags = pIn1->flags | pIn2->flags;
  if( (flags & MEM_Null)!=0 ) goto arithmetic_result_is_null;

  if( (pIn1->flags&MEM_Int) && (pIn2->flags&MEM_Int) ){
    iA = sqlite4_num_to_int64(pIn1->u.num, 0);
    iB = sqlite4_num_to_int64(pIn2->u.num, 0);

    switch( pOp->opcode ){
      case OP_Add:       if( sqlite4AddInt64(&iB,iA) ) goto fp_math;  break;
      case OP_Subtract:  if( sqlite4SubInt64(&iB,iA) ) goto fp_math;  break;
      case OP_Multiply:  if( sqlite4MulInt64(&iB,iA) ) goto fp_math;  break;
      case OP_Divide: {
        if( iA==0 ) goto arithmetic_result_is_null;
        if( iA==-1 && iB==SMALLEST_INT64 ) goto fp_math;
        iB /= iA;
        break;
      }
      default: {
        if( iA==0 ) goto arithmetic_result_is_null;
        if( iA==-1 ) iA = 1;
        iB %= iA;
        break;
      }
    }
    pOut->u.num = sqlite4_num_from_int64(iB);
    MemSetTypeFlag(pOut, MEM_Int);

    break;
  }else{

 fp_math:
    num1 = sqlite4VdbeNumValue(pIn1);
    num2 = sqlite4VdbeNumValue(pIn2);
    switch( pOp->opcode ){
      case OP_Add: 
        pOut->u.num = sqlite4_num_add(num1, num2); break;
      case OP_Subtract: 
        pOut->u.num = sqlite4_num_sub(num2, num1); break;
      case OP_Multiply: 
        pOut->u.num = sqlite4_num_mul(num1, num2); break;
      case OP_Divide: 
        pOut->u.num = sqlite4_num_div(num2, num1); break;
      default: {
        iA = sqlite4_num_to_int64(num1, 0);
        iB = sqlite4_num_to_int64(num2, 0);
        if( iA==0 ) goto arithmetic_result_is_null;
        if( iA==-1 ) iA = 1;
        pOut->u.num = sqlite4_num_from_int64(iB % iA);
        break;
      }
    }

    if( sqlite4_num_isnan(pOut->u.num) ){
      goto arithmetic_result_is_null;
    }else{
      MemSetTypeFlag(pOut, MEM_Real);
      if( (flags & MEM_Real)==0 ){
        sqlite4VdbeIntegerAffinity(pOut);
      }
    }
  }

  break;

arithmetic_result_is_null:
  sqlite4VdbeMemSetNull(pOut);
  break;
}

/* Opcode: CollSeq * * P4
**
** P4 is a pointer to a CollSeq struct. If the next call to a user function
** or aggregate calls sqlite4GetFuncCollSeq(), this collation sequence will
** be returned. This is used by the built-in min(), max() and nullif()
** functions.
**
** The interface used by the implementation of the aforementioned functions
** to retrieve the collation sequence set by this opcode is not available
** publicly, only to user functions defined in func.c.
*/
case OP_CollSeq: {
  assert( pOp->p4type==P4_COLLSEQ );
  break;
}

/* Opcode: Mifunction P1
*/
case OP_KVMethod: {
  assert( pOp[1].opcode==OP_Function );
  break;
};

/* Opcode: Mifunction P1
*/
case OP_Mifunction: {
  pc++;
  pOp++;
  /* fall through to OP_Function */
};

/* Opcode: Function P1 P2 P3 P4 P5
**
** Invoke a user function (P4 is a pointer to a Function structure that
** defines the function) with P5 arguments taken from register P2 and
** successors.  The result of the function is stored in register P3.
** Register P3 must not be one of the function inputs.
**
** P1 is a 32-bit bitmask indicating whether or not each argument to the 
** function was determined to be constant at compile time. If the first
** argument was constant then bit 0 of P1 is set. This is used to determine
** whether meta data associated with a user function argument using the
** sqlite4_auxdata_store() API may be safely retained until the next
** invocation of this opcode.
**
** See also: AggStep and AggFinal
*/
case OP_Function: {
  int i;
  Mem *pArg;
  sqlite4_context ctx;
  sqlite4_value **apVal;
  int n;

  n = pOp->p5;
  apVal = p->apArg;
  assert( apVal || n==0 );
  assert( pOp->p3>0 && pOp->p3<=p->nMem );
  pOut = &aMem[pOp->p3];
  memAboutToChange(p, pOut);

  assert( n==0 || (pOp->p2>0 && pOp->p2+n<=p->nMem+1) );
  assert( pOp->p3<pOp->p2 || pOp->p3>=pOp->p2+n );
  pArg = &aMem[pOp->p2];
  for(i=0; i<n; i++, pArg++){
    assert( memIsValid(pArg) );
    apVal[i] = pArg;
    Deephemeralize(pArg);
    sqlite4VdbeMemStoreType(pArg);
    REGISTER_TRACE(pOp->p2+i, pArg);
  }

  assert( pOp->p4type==P4_FUNCDEF || pOp->p4type==P4_VDBEFUNC );
  if( pOp->p4type==P4_FUNCDEF ){
    ctx.pFunc = pOp->p4.pFunc;
    ctx.pVdbeFunc = 0;
  }else{
    ctx.pVdbeFunc = (VdbeFunc*)pOp->p4.pVdbeFunc;
    ctx.pFunc = ctx.pVdbeFunc->pFunc;
  }

  ctx.s.flags = MEM_Null;
  ctx.s.db = db;
  ctx.s.xDel = 0;
  ctx.s.zMalloc = 0;
  ctx.pFts = 0;
  if( pOp[-1].opcode==OP_Mifunction ){
    ctx.pFts = p->apCsr[pOp[-1].p1]->pFts;
    apVal++;
    n--;
  }

  /* The output cell may already have a buffer allocated. Move
  ** the pointer to ctx.s so in case the user-function can use
  ** the already allocated buffer instead of allocating a new one.
  */
  sqlite4VdbeMemMove(&ctx.s, pOut);
  MemSetTypeFlag(&ctx.s, MEM_Null);

  ctx.isError = 0;
  if( ctx.pFunc->flags & SQLITE4_FUNC_NEEDCOLL ){
    assert( pOp>aOp );
    assert( pOp[-1].p4type==P4_COLLSEQ );
    assert( pOp[-1].opcode==OP_CollSeq );
    ctx.pColl = pOp[-1].p4.pColl;
  }
  (*ctx.pFunc->xFunc)(&ctx, n, apVal); /* IMP: R-24505-23230 */

  /* If any auxiliary data functions have been called by this user function,
  ** immediately call the destructor for any non-static values.
  */
  if( ctx.pVdbeFunc ){
    sqlite4VdbeDeleteAuxData(ctx.pVdbeFunc, pOp->p1);
    pOp->p4.pVdbeFunc = ctx.pVdbeFunc;
    pOp->p4type = P4_VDBEFUNC;
  }

  if( db->mallocFailed ){
    /* Even though a malloc() has failed, the implementation of the
    ** user function may have called an sqlite4_result_XXX() function
    ** to return a value. The following call releases any resources
    ** associated with such a value.
    */
    sqlite4VdbeMemRelease(&ctx.s);
    goto no_mem;
  }

  /* If the function returned an error, throw an exception */
  if( ctx.isError ){
    sqlite4SetString(&p->zErrMsg, db, "%s", 
        (const char *)sqlite4ValueText(&ctx.s, SQLITE4_UTF8)
    );
    rc = ctx.isError;
  }

  /* Copy the result of the function into register P3 */
  sqlite4VdbeChangeEncoding(&ctx.s, encoding);
  sqlite4VdbeMemMove(pOut, &ctx.s);
  if( sqlite4VdbeMemTooBig(pOut) ){
    goto too_big;
  }

#if 0
  /* The app-defined function has done something that as caused this
  ** statement to expire.  (Perhaps the function called sqlite4_exec()
  ** with a CREATE TABLE statement.)
  */
  if( p->expired ) rc = SQLITE4_ABORT;
#endif

  REGISTER_TRACE(pOp->p3, pOut);
  UPDATE_MAX_BLOBSIZE(pOut);
  break;
}

/* Opcode: BitAnd P1 P2 P3 * *
**
** Take the bit-wise AND of the values in register P1 and P2 and
** store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: BitOr P1 P2 P3 * *
**
** Take the bit-wise OR of the values in register P1 and P2 and
** store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: ShiftLeft P1 P2 P3 * *
**
** Shift the integer value in register P2 to the left by the
** number of bits specified by the integer in register P1.
** Store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: ShiftRight P1 P2 P3 * *
**
** Shift the integer value in register P2 to the right by the
** number of bits specified by the integer in register P1.
** Store the result in register P3.
** If either input is NULL, the result is NULL.
*/
case OP_BitAnd:                 /* same as TK_BITAND, in1, in2, out3 */
case OP_BitOr:                  /* same as TK_BITOR, in1, in2, out3 */
case OP_ShiftLeft:              /* same as TK_LSHIFT, in1, in2, out3 */
case OP_ShiftRight: {           /* same as TK_RSHIFT, in1, in2, out3 */
  i64 iA;
  u64 uA;
  i64 iB;
  u8 op;

  pIn1 = &aMem[pOp->p1];
  pIn2 = &aMem[pOp->p2];
  pOut = &aMem[pOp->p3];
  if( (pIn1->flags | pIn2->flags) & MEM_Null ){
    sqlite4VdbeMemSetNull(pOut);
    break;
  }
  iA = sqlite4VdbeIntValue(pIn2);
  iB = sqlite4VdbeIntValue(pIn1);
  op = pOp->opcode;
  if( op==OP_BitAnd ){
    iA &= iB;
  }else if( op==OP_BitOr ){
    iA |= iB;
  }else if( iB!=0 ){
    assert( op==OP_ShiftRight || op==OP_ShiftLeft );

    /* If shifting by a negative amount, shift in the other direction */
    if( iB<0 ){
      assert( OP_ShiftRight==OP_ShiftLeft+1 );
      op = 2*OP_ShiftLeft + 1 - op;
      iB = iB>(-64) ? -iB : 64;
    }

    if( iB>=64 ){
      iA = (iA>=0 || op==OP_ShiftLeft) ? 0 : -1;
    }else{
      memcpy(&uA, &iA, sizeof(uA));
      if( op==OP_ShiftLeft ){
        uA <<= iB;
      }else{
        uA >>= iB;
        /* Sign-extend on a right shift of a negative number */
        if( iA<0 ) uA |= ((((u64)0xffffffff)<<32)|0xffffffff) << (64-iB);
      }
      memcpy(&iA, &uA, sizeof(iA));
    }
  }

  pOut->u.num = sqlite4_num_from_int64(iA);
  MemSetTypeFlag(pOut, MEM_Int);
  break;
}

/* Opcode: AddImm  P1 P2 * * *
** 
** Add the constant P2 to the value in register P1.
** The result is always an integer.
**
** To force any register to be an integer, just add 0.
*/
case OP_AddImm: {            /* in1 */
  pIn1 = &aMem[pOp->p1];
  memAboutToChange(p, pIn1);
  sqlite4VdbeMemIntegerify(pIn1);
  pIn1->u.num = sqlite4_num_add(pIn1->u.num, sqlite4_num_from_int64(pOp->p2));
  break;
}

/* Opcode: MustBeInt P1 P2 * * *
** 
** Force the value in register P1 to be an integer.  If the value
** in P1 is not an integer and cannot be converted into an integer
** without data loss, then jump immediately to P2, or if P2==0
** raise an SQLITE4_MISMATCH exception.
*/
case OP_MustBeInt: {            /* jump, in1 */
  pIn1 = &aMem[pOp->p1];
  applyAffinity(pIn1, SQLITE4_AFF_NUMERIC, encoding);
  if( (pIn1->flags & MEM_Int)==0 ){
    if( pOp->p2==0 ){
      rc = SQLITE4_MISMATCH;
      goto abort_due_to_error;
    }else{
      pc = pOp->p2 - 1;
    }
  }else{
    MemSetTypeFlag(pIn1, MEM_Int);
  }
  break;
}

#ifndef SQLITE4_OMIT_FLOATING_POINT
/* Opcode: RealAffinity P1 * * * *
**
** If register P1 holds an integer convert it to a real value.
**
** This opcode is used when extracting information from a column that
** has REAL affinity.  Such column values may still be stored as
** integers, for space efficiency, but after extraction we want them
** to have only a real value.
*/
case OP_RealAffinity: {                  /* in1 */
  pIn1 = &aMem[pOp->p1];
  if( pIn1->flags & MEM_Int ){
    MemSetTypeFlag(pIn1, MEM_Real);
  }
  break;
}
#endif

#ifndef SQLITE4_OMIT_CAST
/* Opcode: ToText P1 * * * *
**
** Force the value in register P1 to be text.
** If the value is numeric, convert it to a string using the
** equivalent of printf().  Blob values are unchanged and
** are afterwards simply interpreted as text.
**
** A NULL value is not changed by this routine.  It remains NULL.
*/
case OP_ToText: {                  /* same as TK_TO_TEXT, in1 */
  pIn1 = &aMem[pOp->p1];
  memAboutToChange(p, pIn1);
  if( pIn1->flags & MEM_Null ) break;
  assert( MEM_Str==(MEM_Blob>>3) );
  pIn1->flags |= (pIn1->flags&MEM_Blob)>>3;
  applyAffinity(pIn1, SQLITE4_AFF_TEXT, encoding);
  assert( pIn1->flags & MEM_Str || db->mallocFailed );
  pIn1->flags &= ~(MEM_Int|MEM_Real|MEM_Blob);
  UPDATE_MAX_BLOBSIZE(pIn1);
  break;
}

/* Opcode: ToBlob P1 * * * *
**
** Force the value in register P1 to be a BLOB.
** If the value is numeric, convert it to a string first.
** Strings are simply reinterpreted as blobs with no change
** to the underlying data.
**
** A NULL value is not changed by this routine.  It remains NULL.
*/
case OP_ToBlob: {                  /* same as TK_TO_BLOB, in1 */
  pIn1 = &aMem[pOp->p1];
  if( pIn1->flags & MEM_Null ) break;
  if( (pIn1->flags & MEM_Blob)==0 ){
    applyAffinity(pIn1, SQLITE4_AFF_TEXT, encoding);
    assert( pIn1->flags & MEM_Str || db->mallocFailed );
    MemSetTypeFlag(pIn1, MEM_Blob);
  }else{
    pIn1->flags &= ~(MEM_TypeMask&~MEM_Blob);
  }
  UPDATE_MAX_BLOBSIZE(pIn1);
  break;
}

/* Opcode: ToNumeric P1 * * * *
**
** Force the value in register P1 to be numeric (either an
** integer or a floating-point number.)
** If the value is text or blob, try to convert it to an using the
** equivalent of atoi() or atof() and store 0 if no such conversion 
** is possible.
**
** A NULL value is not changed by this routine.  It remains NULL.
*/
case OP_ToNumeric: {                  /* same as TK_TO_NUMERIC, in1 */
  pIn1 = &aMem[pOp->p1];
  sqlite4VdbeMemNumerify(pIn1);
  break;
}
#endif /* SQLITE4_OMIT_CAST */

/* Opcode: ToInt P1 * * * *
**
** Force the value in register P1 to be an integer.  If
** The value is currently a real number, drop its fractional part.
** If the value is text or blob, try to convert it to an integer using the
** equivalent of atoi() and store 0 if no such conversion is possible.
**
** A NULL value is not changed by this routine.  It remains NULL.
*/
case OP_ToInt: {                  /* same as TK_TO_INT, in1 */
  pIn1 = &aMem[pOp->p1];
  if( (pIn1->flags & MEM_Null)==0 ){
    sqlite4VdbeMemIntegerify(pIn1);
  }
  break;
}

#if !defined(SQLITE4_OMIT_CAST) && !defined(SQLITE4_OMIT_FLOATING_POINT)
/* Opcode: ToReal P1 * * * *
**
** Force the value in register P1 to be a floating point number.
** If The value is currently an integer, convert it.
** If the value is text or blob, try to convert it to an integer using the
** equivalent of atoi() and store 0.0 if no such conversion is possible.
**
** A NULL value is not changed by this routine.  It remains NULL.
*/
case OP_ToReal: {                  /* same as TK_TO_REAL, in1 */
  pIn1 = &aMem[pOp->p1];
  memAboutToChange(p, pIn1);
  sqlite4VdbeMemNumerify(pIn1);
  pIn1->flags |= MEM_Real;
  pIn1->flags &= ~MEM_Int;
  break;
}
#endif /* !defined(SQLITE4_OMIT_CAST) && !defined(SQLITE4_OMIT_FLOATING_POINT) */

/* Opcode: Lt P1 P2 P3 P4 P5
**
** Compare the values in register P1 and P3.  If reg(P3)<reg(P1) then
** jump to address P2.  
**
** If the SQLITE4_JUMPIFNULL bit of P5 is set and either reg(P1) or
** reg(P3) is NULL then take the jump.  If the SQLITE4_JUMPIFNULL 
** bit is clear then fall through if either operand is NULL.
**
** The SQLITE4_AFF_MASK portion of P5 must be an affinity character -
** SQLITE4_AFF_TEXT, SQLITE4_AFF_INTEGER, and so forth. An attempt is made 
** to coerce both inputs according to this affinity before the
** comparison is made. If the SQLITE4_AFF_MASK is 0x00, then numeric
** affinity is used. Note that the affinity conversions are stored
** back into the input registers P1 and P3.  So this opcode can cause
** persistent changes to registers P1 and P3.
**
** Once any conversions have taken place, and neither value is NULL, 
** the values are compared. If both values are blobs then memcmp() is
** used to determine the results of the comparison.  If both values
** are text, then the appropriate collating function specified in
** P4 is  used to do the comparison.  If P4 is not specified then
** memcmp() is used to compare text string.  If both values are
** numeric, then a numeric comparison is used. If the two values
** are of different types, then numbers are considered less than
** strings and strings are considered less than blobs.
**
** If the SQLITE4_STOREP2 bit of P5 is set, then do not jump.  Instead,
** store a boolean result (either 0, or 1, or NULL) in register P2.
*/
/* Opcode: Ne P1 P2 P3 P4 P5
**
** This works just like the Lt opcode except that the jump is taken if
** the operands in registers P1 and P3 are not equal.  See the Lt opcode for
** additional information.
**
** If SQLITE4_NULLEQ is set in P5 then the result of comparison is always either
** true or false and is never NULL.  If both operands are NULL then the result
** of comparison is false.  If either operand is NULL then the result is true.
** If neither operand is NULL the result is the same as it would be if
** the SQLITE4_NULLEQ flag were omitted from P5.
*/
/* Opcode: Eq P1 P2 P3 P4 P5
**
** This works just like the Lt opcode except that the jump is taken if
** the operands in registers P1 and P3 are equal.
** See the Lt opcode for additional information.
**
** If SQLITE4_NULLEQ is set in P5 then the result of comparison is always either
** true or false and is never NULL.  If both operands are NULL then the result
** of comparison is true.  If either operand is NULL then the result is false.
** If neither operand is NULL the result is the same as it would be if
** the SQLITE4_NULLEQ flag were omitted from P5.
*/
/* Opcode: Le P1 P2 P3 P4 P5
**
** This works just like the Lt opcode except that the jump is taken if
** the content of register P3 is less than or equal to the content of
** register P1.  See the Lt opcode for additional information.
*/
/* Opcode: Gt P1 P2 P3 P4 P5
**
** This works just like the Lt opcode except that the jump is taken if
** the content of register P3 is greater than the content of
** register P1.  See the Lt opcode for additional information.
*/
/* Opcode: Ge P1 P2 P3 P4 P5
**
** This works just like the Lt opcode except that the jump is taken if
** the content of register P3 is greater than or equal to the content of
** register P1.  See the Lt opcode for additional information.
*/
case OP_Eq:               /* same as TK_EQ, jump, in1, in3 */
case OP_Ne:               /* same as TK_NE, jump, in1, in3 */
case OP_Lt:               /* same as TK_LT, jump, in1, in3 */
case OP_Le:               /* same as TK_LE, jump, in1, in3 */
case OP_Gt:               /* same as TK_GT, jump, in1, in3 */
case OP_Ge: {             /* same as TK_GE, jump, in1, in3 */
  int res;            /* Result of the comparison of pIn1 against pIn3 */
  char affinity;      /* Affinity to use for comparison */
  u16 flags1;         /* Copy of initial value of pIn1->flags */
  u16 flags3;         /* Copy of initial value of pIn3->flags */

  pIn1 = &aMem[pOp->p1];
  pIn3 = &aMem[pOp->p3];
  flags1 = pIn1->flags;
  flags3 = pIn3->flags;
  if( (flags1 | flags3)&MEM_Null ){
    /* One or both operands are NULL */
    if( pOp->p5 & SQLITE4_NULLEQ ){
      /* If SQLITE4_NULLEQ is set (which will only happen if the operator is
      ** OP_Eq or OP_Ne) then take the jump or not depending on whether
      ** or not both operands are null.
      */
      assert( pOp->opcode==OP_Eq || pOp->opcode==OP_Ne );
      res = (flags1 & flags3 & MEM_Null)==0;
    }else{
      /* SQLITE4_NULLEQ is clear and at least one operand is NULL,
      ** then the result is always NULL.
      ** The jump is taken if the SQLITE4_JUMPIFNULL bit is set.
      */
      if( pOp->p5 & SQLITE4_STOREP2 ){
        pOut = &aMem[pOp->p2];
        MemSetTypeFlag(pOut, MEM_Null);
        REGISTER_TRACE(pOp->p2, pOut);
      }else if( pOp->p5 & SQLITE4_JUMPIFNULL ){
        pc = pOp->p2-1;
      }
      break;
    }
  }else{
    /* Neither operand is NULL.  Do a comparison. */
    affinity = pOp->p5 & SQLITE4_AFF_MASK;
    if( affinity ){
      applyAffinity(pIn1, affinity, encoding);
      applyAffinity(pIn3, affinity, encoding);
      if( db->mallocFailed ) goto no_mem;
    }

    assert( pOp->p4type==P4_COLLSEQ || pOp->p4.pColl==0 );
    rc = sqlite4MemCompare(pIn3, pIn1, pOp->p4.pColl, &res);
  }
  switch( pOp->opcode ){
    case OP_Eq:    res = res==0;     break;
    case OP_Ne:    res = res!=0;     break;
    case OP_Lt:    res = res<0;      break;
    case OP_Le:    res = res<=0;     break;
    case OP_Gt:    res = res>0;      break;
    default:       res = res>=0;     break;
  }

  if( pOp->p5 & SQLITE4_STOREP2 ){
    pOut = &aMem[pOp->p2];
    memAboutToChange(p, pOut);
    MemSetTypeFlag(pOut, MEM_Int);
    pOut->u.num = sqlite4_num_from_int64(res);
    REGISTER_TRACE(pOp->p2, pOut);
  }else if( res ){
    pc = pOp->p2-1;
  }

  /* Undo any changes made by applyAffinity() to the input registers. */
  pIn1->flags = (pIn1->flags&~MEM_TypeMask) | (flags1&MEM_TypeMask);
  pIn3->flags = (pIn3->flags&~MEM_TypeMask) | (flags3&MEM_TypeMask);
  break;
}

/* Opcode: Permutation P1 * * P4 *
**
** Set the permutation used by the OP_Compare operator to be the array
** of integers in P4.  P4 will contain exactly P1 elements.  The P1
** parameter is used only for printing the P4 array when debugging.
**
** The permutation is only valid until the next OP_Permutation, OP_Compare,
** OP_Halt, or OP_ResultRow.  Typically the OP_Permutation should occur
** immediately prior to the OP_Compare.
*/
case OP_Permutation: {
  assert( pOp->p4type==P4_INTARRAY );
  assert( pOp->p4.ai );
  aPermute = pOp->p4.ai;
  break;
}

/* Opcode: Compare P1 P2 P3 P4 *
**
** Compare two vectors of registers in reg(P1)..reg(P1+P3-1) (call this
** vector "A") and in reg(P2)..reg(P2+P3-1) ("B").  Save the result of
** the comparison for use by the next OP_Jump instruct.
**
** P4 is a KeyInfo structure that defines collating sequences and sort
** orders for the comparison.  The permutation applies to registers
** only.  The KeyInfo elements are used sequentially.
**
** The comparison is a sort comparison, so NULLs compare equal,
** NULLs are less than numbers, numbers are less than strings,
** and strings are less than blobs.
*/
case OP_Compare: {
  int n;
  int i;
  int p1;
  int p2;
  const KeyInfo *pKeyInfo;
  int idx;
  CollSeq *pColl;    /* Collating sequence to use on this term */
  int bRev;          /* True for DESCENDING sort order */

  n = pOp->p3;
  pKeyInfo = pOp->p4.pKeyInfo;
  assert( n>0 );
  assert( pKeyInfo!=0 );
  p1 = pOp->p1;
  p2 = pOp->p2;
#if SQLITE4_DEBUG
  if( aPermute ){
    int k, mx = 0;
    for(k=0; k<n; k++) if( aPermute[k]>mx ) mx = aPermute[k];
    assert( p1>0 && p1+mx<=p->nMem+1 );
    assert( p2>0 && p2+mx<=p->nMem+1 );
  }else{
    assert( p1>0 && p1+n<=p->nMem+1 );
    assert( p2>0 && p2+n<=p->nMem+1 );
  }
#endif /* SQLITE4_DEBUG */
  for(i=0; i<n; i++){
    idx = aPermute ? aPermute[i] : i;
    assert( memIsValid(&aMem[p1+idx]) );
    assert( memIsValid(&aMem[p2+idx]) );
    REGISTER_TRACE(p1+idx, &aMem[p1+idx]);
    REGISTER_TRACE(p2+idx, &aMem[p2+idx]);
    assert( i<pKeyInfo->nField );
    pColl = pKeyInfo->aColl[i];
    bRev = pKeyInfo->aSortOrder[i];
    rc = sqlite4MemCompare(&aMem[p1+idx], &aMem[p2+idx], pColl, &iCompare);
    if( iCompare ){
      if( bRev ) iCompare = -iCompare;
      break;
    }
  }
  aPermute = 0;
  break;
}

/* Opcode: Jump P1 P2 P3 * *
**
** Jump to the instruction at address P1, P2, or P3 depending on whether
** in the most recent OP_Compare instruction the P1 vector was less than
** equal to, or greater than the P2 vector, respectively.
*/
case OP_Jump: {             /* jump */
  if( iCompare<0 ){
    pc = pOp->p1 - 1;
  }else if( iCompare==0 ){
    pc = pOp->p2 - 1;
  }else{
    pc = pOp->p3 - 1;
  }
  break;
}

/* Opcode: And P1 P2 P3 * *
**
** Take the logical AND of the values in registers P1 and P2 and
** write the result into register P3.
**
** If either P1 or P2 is 0 (false) then the result is 0 even if
** the other input is NULL.  A NULL and true or two NULLs give
** a NULL output.
*/
/* Opcode: Or P1 P2 P3 * *
**
** Take the logical OR of the values in register P1 and P2 and
** store the answer in register P3.
**
** If either P1 or P2 is nonzero (true) then the result is 1 (true)
** even if the other input is NULL.  A NULL and false or two NULLs
** give a NULL output.
*/
case OP_And:              /* same as TK_AND, in1, in2, out3 */
case OP_Or: {             /* same as TK_OR, in1, in2, out3 */
  int v1;    /* Left operand:  0==FALSE, 1==TRUE, 2==UNKNOWN or NULL */
  int v2;    /* Right operand: 0==FALSE, 1==TRUE, 2==UNKNOWN or NULL */

  pIn1 = &aMem[pOp->p1];
  if( pIn1->flags & MEM_Null ){
    v1 = 2;
  }else{
    v1 = sqlite4VdbeIntValue(pIn1)!=0;
  }
  pIn2 = &aMem[pOp->p2];
  if( pIn2->flags & MEM_Null ){
    v2 = 2;
  }else{
    v2 = sqlite4VdbeIntValue(pIn2)!=0;
  }
  if( pOp->opcode==OP_And ){
    static const unsigned char and_logic[] = { 0, 0, 0, 0, 1, 2, 0, 2, 2 };
    v1 = and_logic[v1*3+v2];
  }else{
    static const unsigned char or_logic[] = { 0, 1, 2, 1, 1, 1, 2, 1, 2 };
    v1 = or_logic[v1*3+v2];
  }
  pOut = &aMem[pOp->p3];
  if( v1==2 ){
    MemSetTypeFlag(pOut, MEM_Null);
  }else{
    pOut->u.num = sqlite4_num_from_int64(v1);
    MemSetTypeFlag(pOut, MEM_Int);
  }
  break;
}

/* Opcode: Not P1 P2 * * *
**
** Interpret the value in register P1 as a boolean value.  Store the
** boolean complement in register P2.  If the value in register P1 is 
** NULL, then a NULL is stored in P2.
*/
case OP_Not: {                /* same as TK_NOT, in1, out2 */
  pIn1 = &aMem[pOp->p1];
  pOut = &aMem[pOp->p2];
  if( pIn1->flags & MEM_Null ){
    sqlite4VdbeMemSetNull(pOut);
  }else{
    sqlite4VdbeMemSetInt64(pOut, !sqlite4VdbeIntValue(pIn1));
  }
  break;
}

/* Opcode: BitNot P1 P2 * * *
**
** Interpret the content of register P1 as an integer.  Store the
** ones-complement of the P1 value into register P2.  If P1 holds
** a NULL then store a NULL in P2.
*/
case OP_BitNot: {             /* same as TK_BITNOT, in1, out2 */
  pIn1 = &aMem[pOp->p1];
  pOut = &aMem[pOp->p2];
  if( pIn1->flags & MEM_Null ){
    sqlite4VdbeMemSetNull(pOut);
  }else{
    sqlite4VdbeMemSetInt64(pOut, ~sqlite4VdbeIntValue(pIn1));
  }
  break;
}

/* Opcode: Once P1 P2 * * *
**
** Check if OP_Once flag P1 is set. If so, jump to instruction P2. Otherwise,
** set the flag and fall through to the next instruction.
**
** See also: JumpOnce
*/
case OP_Once: {             /* jump */
  assert( pOp->p1<p->nOnceFlag );
  if( p->aOnceFlag[pOp->p1] ){
    pc = pOp->p2-1;
  }else{
    p->aOnceFlag[pOp->p1] = 1;
  }
  break;
}

/* Opcode: If P1 P2 P3 * *
**
** Jump to P2 if the value in register P1 is true.  The value
** is considered true if it is numeric and non-zero.  If the value
** in P1 is NULL then take the jump if P3 is non-zero.
*/
/* Opcode: IfNot P1 P2 P3 * *
**
** Jump to P2 if the value in register P1 is False.  The value
** is considered false if it has a numeric value of zero.  If the value
** in P1 is NULL then take the jump if P3 is zero.
*/
case OP_If:                 /* jump, in1 */
case OP_IfNot: {            /* jump, in1 */
  int c;
  pIn1 = &aMem[pOp->p1];
  if( pIn1->flags & MEM_Null ){
    c = pOp->p3;
  }else{
    c = sqlite4VdbeNumValue(pIn1).m!=0;
    if( pOp->opcode==OP_IfNot ) c = !c;
  }
  if( c ){
    pc = pOp->p2-1;
  }
  break;
}

/* Opcode: IsNull P1 P2 P3 * *
**
** P1 is the first in an array of P3 registers. Or, if P3 is 0, the first
** in an array of a single register. If any registers in the array are
** NULL, jump to instruction P2.
*/
case OP_IsNull: {            /* same as TK_ISNULL, jump, in1 */
  Mem *pEnd;
  pIn1 = &aMem[pOp->p1];
  pEnd = &aMem[pOp->p1+pOp->p3];

  do {
    if( (pIn1->flags & MEM_Null)!=0 ){
      pc = pOp->p2 - 1;
      break;
    }
  }while( (++pIn1)<pEnd );

  break;
}

/* Opcode: NotNull P1 P2 * * *
**
** Jump to P2 if the value in register P1 is not NULL.  
*/
case OP_NotNull: {            /* same as TK_NOTNULL, jump, in1 */
  pIn1 = &aMem[pOp->p1];
  if( (pIn1->flags & MEM_Null)==0 ){
    pc = pOp->p2 - 1;
  }
  break;
}

/* Opcode: Column P1 P2 P3 P4 P5
**
** Interpret the data that cursor P1 points to as a structure built using
** the MakeRecord instruction.  (See the MakeRecord opcode for additional
** information about the format of the data.)  Extract the P2-th column
** from this record.  If there are less that (P2+1) 
** values in the record, extract a NULL.
**
** The value extracted is stored in register P3.
**
** If the column contains fewer than P2 fields, then extract a NULL.  Or,
** if the P4 argument is a P4_MEM use the value of the P4 argument as
** the result.
**
** If the OPFLAG_CLEARCACHE bit is set on P5 and P1 is a pseudo-table cursor,
** then the cache of the cursor is reset prior to extracting the column.
** The first OP_Column against a pseudo-table after the value of the content
** register has changed should have this bit set.
*/
case OP_Column: {
  int p1;                   /* Index of VdbeCursor to decode */
  int mxField;              /* Maximum column number */
  VdbeCursor *pC;           /* The VDBE cursor */
  Mem *pDest;               /* Where to write the results */
  Mem *pDefault;            /* Default value from P4 */

  p1 = pOp->p1;
  assert( p1<p->nCursor );
  assert( pOp->p3>0 && pOp->p3<=p->nMem );
  pDest = &aMem[pOp->p3];
  memAboutToChange(p, pDest);
  pC = p->apCsr[p1];
  assert( pC!=0 );
  assert( pC->iRoot!=KVSTORE_ROOT );
#ifndef SQLITE4_OMIT_VIRTUALTABLE
  assert( pC->pVtabCursor==0 );
#endif
  if( pC->pDecoder==0 ){
    mxField = pC->nField;
    if( pC->pKeyInfo && pC->pKeyInfo->nData ) mxField = pC->pKeyInfo->nData;
    rc = sqlite4VdbeDecoderCreate(db, pC, 0, mxField, &pC->pDecoder);
    pC->rowChnged = 1;
  }
  if( rc==SQLITE4_OK ){
    pDefault = (pOp->p4type==P4_MEM) ? pOp->p4.pMem : 0;
    rc = sqlite4VdbeDecoderGetColumn(pC->pDecoder, pOp->p2, pDefault, pDest);
  }else{
    sqlite4VdbeMemSetNull(pDest);
  }
  UPDATE_MAX_BLOBSIZE(pDest);
  REGISTER_TRACE(pOp->p3, pDest);
  break;
}

/* Opcode:  MakeKey  P1 P2 P3 P4 P5
**
** Encode the values in registers P1..P1+P2-1 using the key encoding
** and write the result into register P3.  The cursor used for the encoding
** is given by the P4 value which must be an integer (P4_INT32).
**
** If the OPFLAG_SEQCOUNT bit of P5 is set, then a sequence number 
** (unique within the cursor) is appended to the record. The sole purpose
** of this is to ensure that the key blob is unique within the cursor table.
*/
/* Opcode:  MakeRecord  P1 P2 P3 P4 P5
**
** Encode the values in registers P1..P1+P2-1 using the data encoding
** and write the result into register P3.  Apply affinities in P4 prior
** to performing the encoding.
**
** If the OPFLAG_USEKEY bit of P5 is set and this opcode immediately follows
** an MakeKey opcode, then the data encoding generated may try to refer
** to content in the previously generated key in order to make the encoding
** smaller.
*/
case OP_MakeKey:
case OP_MakeRecord: {
  VdbeCursor *pC;        /* The cursor for OP_MakeKey */
  Mem *pData0;           /* First field to be combined into the record */
  Mem *pLast;            /* Last field of the record */
  Mem *pMem;             /* For looping over inputs */
  Mem *pOut;             /* Where to store results */
  int nIn;               /* Number of input values to be encoded */
  char *zAffinity;       /* The affinity string */
  u8 *aRec;              /* The constructed key or value */
  int nRec;              /* Size of aRec[] in bytes */
  int bRepeat;           /* True to loop to the next opcode */
  u8 aSeq[10];           /* Encoded sequence number */
  int nSeq;              /* Size of sequence number in bytes */
  u64 iSeq;              /* Sequence number, if any */
  
  
  do{
    bRepeat = 0;
    zAffinity = pOp->p4type==P4_INT32 ? 0 : pOp->p4.z;
    assert( pOp->p1>0 && pOp->p2>0 && pOp->p2+pOp->p1<=p->nMem+1 );
    pData0 = &aMem[pOp->p1];
    nIn = pOp->p2;
    pLast = &pData0[nIn-1];
    assert( pOp->p3>0 && pOp->p3<=p->nMem );
    pOut = &aMem[pOp->p3];
    memAboutToChange(p, pOut);
    aRec = 0;
    nSeq = 0;

    /* Apply affinities */
    if( zAffinity ){
      for(pMem=pData0; pMem<=pLast; pMem++){
        assert( memIsValid(pMem) );
        applyAffinity(pMem, *(zAffinity++), encoding);
      }
    }

    if( pOp->opcode==OP_MakeKey ){
      assert( pOp->p4type==P4_INT32 );
      assert( pOp->p4.i>=0 && pOp->p4.i<p->nCursor );
      pC = p->apCsr[pOp->p4.i];

      /* If P4 contains OPFLAG_SEQCOUNT, encode the sequence number blob to be
      ** appended to the end of the key.  Variable nSeq is set to the number
      ** of bytes in the encoded key.  A non-standard encoding is used (not
      ** the usual varint encoding) so that the OP_GrpCompare opcode can easily
      ** back up over the sequence count to find the true end of the key.
      */
      if( pOp->p5 & OPFLAG_SEQCOUNT ){
        iSeq = pC->seqCount++;
        do {
          nSeq++;
          aSeq[sizeof(aSeq)-nSeq] = (u8)(iSeq & 0x007F);
          iSeq = iSeq >> 7;
        }while( iSeq );
        aSeq[sizeof(aSeq)-nSeq] |= 0x80;
      }
        printf("/**\tpC->iRoot: %d\n", pC->iRoot);
        printf("/**\tKeyInfo: \n");
        printf("/**        nField: %d\n", pC->pKeyInfo->nField);
        printf("/**        nPK: %d\n", pC->pKeyInfo->nPK);
        printf("/**        nData: %d\n", pC->pKeyInfo->nData);
      /* Generate the key encoding */
      rc = sqlite4VdbeEncodeKey(
        db, pData0, nIn, pC->iRoot, pC->pKeyInfo, &aRec, &nRec, nSeq
      );
      
      if( pOp[1].opcode==OP_MakeRecord ){
        pc++;
        pOp++;
        bRepeat = 1;
      }
    }else{
      assert( pOp->opcode==OP_MakeRecord );
      rc = sqlite4VdbeEncodeData(db, pData0, aPermute, nIn, &aRec, &nRec);
      aPermute = 0;
    } 

    /* Store the result */
    if( rc!=SQLITE4_OK ){
      sqlite4DbFree(db, aRec);
    }else{
      if( nSeq ) memcpy(&aRec[nRec], &aSeq[sizeof(aSeq)-nSeq], nSeq);
      rc = sqlite4VdbeMemSetStr(pOut, (char *)aRec, nRec+nSeq, 0,
                                SQLITE4_DYNAMIC, 0);
      REGISTER_TRACE(pOp->p3, pOut);
      UPDATE_MAX_BLOBSIZE(pOut);
    }
  }while( rc==SQLITE4_OK && bRepeat );
  break;
}

/* Opcode: Affinity P1 P2 * P4 *
**
** Apply affinities to a range of P2 registers starting with P1.
**
** P4 is a string that is P2 characters long. The nth character of the
** string indicates the column affinity that should be used for the nth
** memory cell in the range.
*/
case OP_Affinity: {
  const char *zAffinity;   /* The affinity to be applied */
  Mem *pEnd;

  zAffinity = pOp->p4.z;
  assert( zAffinity!=0 );
  assert( sqlite4Strlen30(zAffinity)>=pOp->p2 );

  pEnd = &aMem[pOp->p2+pOp->p1];
  for(pIn1=&aMem[pOp->p1]; pIn1<pEnd; pIn1++){
    assert( memIsValid(pIn1) );
    memAboutToChange(p, pIn1);
    applyAffinity(pIn1, *(zAffinity++), encoding);
    REGISTER_TRACE(pIn1-aMem, pIn1);
  }

  break;
}

/* Opcode: Count P1 P2 * * *
**
** Store the number of entries (an integer value) in the table or index 
** opened by cursor P1 in register P2
*/
case OP_Count: {         /* out2-prerelease */
  i64 nEntry;
  VdbeCursor *pC;
  
  pC = p->apCsr[pOp->p1];
  rc = sqlite4VdbeSeekEnd(pC, +1);
  nEntry = 0;
  while( rc!=SQLITE4_NOTFOUND ){
    nEntry++;
    rc = sqlite4VdbeNext(pC);
  }
  sqlite4VdbeMemSetInt64(pOut, nEntry);
  if( rc==SQLITE4_NOTFOUND ) rc = SQLITE4_OK;
  break;
}

/* Opcode: Savepoint P1 * * P4 *
**
** This opcode is used to implement the SQL BEGIN, COMMIT, ROLLBACK,
** SAVEPOINT, RELEASE and ROLLBACK TO commands. As follows:
**
**     sql command      p1     p4
**     -------------------------------------
**     BEGIN            0      0
**     COMMIT           1      0
**     ROLLBACK         2      0
**     SAVEPOINT        0      <name of savepoint to open>
**     RELEASE          1      <name of savepoint to release>
**     ROLLBACK TO      2      <name of savepoint to rollback>
*/
case OP_Savepoint: {
  int iSave;
  Savepoint *pSave;               /* Savepoint object operated upon */
  const char *zSave;              /* Name of savepoint (or NULL for trans.) */
  int nSave;                      /* Size of zSave in bytes */
  int iOp;                        /* SAVEPOINT_XXX operation */
  const char *zErr;               /* Static error message */

  zErr = 0;
  zSave = pOp->p4.z;
  nSave = zSave ? sqlite4Strlen30(zSave) : 0;
  iOp = pOp->p1;
  assert( pOp->p1==SAVEPOINT_BEGIN
       || pOp->p1==SAVEPOINT_RELEASE
       || pOp->p1==SAVEPOINT_ROLLBACK
  );

  if( iOp==SAVEPOINT_BEGIN ){
    if( zSave==0 && db->pSavepoint ){
      /* If zSave==0 this is a "BEGIN" command. Return an error if there is
      ** already an open transaction in this case.  */
      zErr = "cannot start a transaction within a transaction";
    }else{
      pSave = (Savepoint *)sqlite4DbMallocZero(db, nSave+1+sizeof(Savepoint));
      if( pSave==0 ) break;
      if( zSave ){
        pSave->zName = (char *)&pSave[1];
        memcpy(pSave->zName, zSave, nSave);
      }
      pSave->pNext = db->pSavepoint;
      pSave->nDeferredCons = db->nDeferredCons;
      db->pSavepoint = pSave;
      db->nSavepoint++;
    }
  }

  else{
    /* Determine which of the zero or more nested savepoints (if any) to 
    ** commit or rollback to. This block sets variable pSave to point
    ** to the Savepoint object and iSave to the kvstore layer transaction
    ** number. For example, to commit or rollback the top level transaction
    ** iSave==2.  */
    iSave = db->nSavepoint+1;
    for(pSave=db->pSavepoint; pSave; pSave=pSave->pNext){
      if( zSave ){
        if( pSave->zName && 0==sqlite4_stricmp(zSave, pSave->zName) ) break;
      }else{
        if( pSave->pNext==0 ) break;
      }
      iSave--;
    }

    if( pSave==0 ){
      if( zSave ){
        sqlite4SetString(&p->zErrMsg, db, "no such savepoint: %s", zSave);
        rc = SQLITE4_ERROR;
      }else if( iOp==SAVEPOINT_RELEASE ){
        zErr = "cannot commit - no transaction is active";
      }else{
        zErr = "cannot rollback - no transaction is active";
      }
    }else{

      /* If this is an attempt to commit the top level transaction, check
      ** that there are no outstanding deferred foreign key violations. If
      ** there are, return an SQLITE4_CONSTRAINT error. Do not release any
      ** savepoints in this case.  */
      if( iOp==SAVEPOINT_RELEASE && iSave==2 ){
        rc = sqlite4VdbeCheckFk(p, 1);
        if( rc!=SQLITE4_OK ) break;
      }

      if( iOp==SAVEPOINT_RELEASE ){
        rc = sqlite4VdbeCommit(db, iSave-1);
      }else{
        rc = sqlite4VdbeRollback(db, iSave-(zSave==0));
      }
    }
  }

  if( zErr ){
    sqlite4SetString(&p->zErrMsg, db, "%s", zErr);
    rc = SQLITE4_ERROR;
  }
  break;
}

/* Opcode: Transaction P1 P2 * * *
**
** Begin a transaction.
**
** P1 is the index of the database file on which the transaction is
** started.  Index 0 is the main database file and index 1 is the
** file used for temporary tables.  Indices of 2 or more are used for
** attached databases.
**
** If P2 is non-zero, then a write-transaction is started.  If P2 is zero
** then a read-transaction is started.
**
** If a write-transaction is started and the Vdbe.needSavepoint flag is
** true (this flag is set if the Vdbe may modify more than one row and may
** throw an ABORT exception), a statement transaction may also be opened.
** More specifically, a statement transaction is opened iff the database
** connection is currently not in autocommit mode, or if there are other
** active statements. A statement transaction allows the affects of this
** VDBE to be rolled back after an error without having to roll back the
** entire transaction. If no error is encountered, the statement transaction
** will automatically commit when the VDBE halts.
*/
case OP_Transaction: {
  Db *pDb;
  KVStore *pKV;
  int bStmt;                      /* True to open statement transaction */
  int iLevel;                     /* Savepoint level to open */

  assert( pOp->p1>=0 && pOp->p1<db->nDb );
  pDb = &db->aDb[pOp->p1];
  pKV = pDb->pKV;
  if( pKV ){
    if( pOp->p2==0 ){
      /* Read transaction needed.  Start if we are not already in one. */
      if( pKV->iTransLevel==0 ){
        rc = sqlite4KVStoreBegin(pKV, 1);
      }
    }else{
      /* A write transaction is needed */
      iLevel = db->nSavepoint + 1;
      if( iLevel<2 ) iLevel = 2;
      bStmt = db->pSavepoint && (p->needSavepoint || db->activeVdbeCnt>1);
      if( pKV->iTransLevel<iLevel ){
        rc = sqlite4KVStoreBegin(pKV, iLevel);
      }
      if( rc==SQLITE4_OK && bStmt ){
        rc = sqlite4KVStoreBegin(pKV, pKV->iTransLevel+1);
        if( rc==SQLITE4_OK ){
          p->stmtTransMask |= ((yDbMask)1)<<pOp->p1;
        }
        p->nStmtDefCons = db->nDeferredCons;
      }
    }
  }
  break;
}

/* Opcode: ReadCookie P1 P2 * * *
**
** Read the schema cookie from database P1 and write it into register P2.
**
** There must be a read-lock on the database (either a transaction
** must be started or there must be an open cursor) before
** executing this instruction.
*/
case OP_ReadCookie: {               /* out2-prerelease */
  unsigned int iMeta;
  KVStore *pKV;

  assert( pOp->p1>=0 && pOp->p1<db->nDb );
  pKV = db->aDb[pOp->p1].pKV;
  rc = sqlite4KVStoreGetSchema(pKV, &iMeta);
  pOut->u.num = sqlite4_num_from_int64(iMeta);
  MemSetTypeFlag(pOut, MEM_Int);
  break;
}

/* Opcode: SetCookie P1 P2 P3 * *
**
** Write the content of register P3 (interpreted as an integer)
** into cookie number P2 of database P1.  P2==1 is the schema version.  
** P2==2 is the database format. P2==3 is the recommended pager cache 
** size, and so forth.  P1==0 is the main database file and P1==1 is the 
** database file used to store temporary tables.
**
** A transaction must be started before executing this opcode.
*/
case OP_SetCookie: {       /* in3 */
  Db *pDb;
  i64 v;

  assert( pOp->p1>=0 && pOp->p1<db->nDb );
  pDb = &db->aDb[pOp->p1];
  pIn3 = &aMem[pOp->p3];
  sqlite4VdbeMemIntegerify(pIn3);
  v = sqlite4_num_to_int64(pIn3->u.num, 0);
  rc = sqlite4KVStorePutSchema(pDb->pKV, (u32)v);
  pDb->pSchema->schema_cookie = (int)v;
  db->flags |= SQLITE4_InternChanges;
  if( pOp->p1==1 ){
    /* Invalidate all prepared statements whenever the TEMP database
    ** schema is changed.  Ticket #1644 */
    sqlite4ExpirePreparedStatements(db);
    p->expired = 0;
  }
  break;
}

/* Opcode: VerifyCookie P1 P2 P3 * *
**
** Check the value of global database parameter number 0 (the
** schema version) and make sure it is equal to P2 and that the
** generation counter on the local schema parse equals P3.
**
** P1 is the database number which is 0 for the main database file
** and 1 for the file holding temporary tables and some higher number
** for auxiliary databases.
**
** The cookie changes its value whenever the database schema changes.
** This operation is used to detect when that the cookie has changed
** and that the current process needs to reread the schema.
**
** Either a transaction needs to have been started or an OP_Open needs
** to be executed (to establish a read lock) before this opcode is
** invoked.
*/
case OP_VerifyCookie: {
  unsigned int iMeta;
  int iGen;
  KVStore *pKV;

  assert( pOp->p1>=0 && pOp->p1<db->nDb );
  pKV = db->aDb[pOp->p1].pKV;
  if( pKV ){
    rc = sqlite4KVStoreGetSchema(pKV, &iMeta);
    if( rc ) break;
    iGen = db->aDb[pOp->p1].pSchema->iGeneration;
  }else{
    iGen = iMeta = 0;
  }
  if( iMeta!=pOp->p2 || iGen!=pOp->p3 ){
    sqlite4DbFree(db, p->zErrMsg);
    p->zErrMsg = sqlite4DbStrDup(db, "database schema has changed");
    /* If the schema-cookie from the database file matches the cookie 
    ** stored with the in-memory representation of the schema, do
    ** not reload the schema from the database file.
    **
    ** If virtual-tables are in use, this is not just an optimization.
    ** Often, v-tables store their data in other SQLite tables, which
    ** are queried from within xNext() and other v-table methods using
    ** prepared queries. If such a query is out-of-date, we do not want to
    ** discard the database schema, as the user code implementing the
    ** v-table would have to be ready for the sqlite4_vtab structure itself
    ** to be invalidated whenever sqlite4_step() is called from within 
    ** a v-table method.
    */
    if( db->aDb[pOp->p1].pSchema->schema_cookie!=iMeta ){
      sqlite4ResetInternalSchema(db, pOp->p1);
    }

    p->expired = 1;
    rc = SQLITE4_SCHEMA;
  }
  break;
}

/* Opcode: OpenRead P1 P2 P3 P4 P5
**
** Open a read-only cursor for the database table whose root page is
** P2 in a database file.  The database file is determined by P3. 
** P3==0 means the main database, P3==1 means the database used for 
** temporary tables, and P3>1 means used the corresponding attached
** database.  Give the new cursor an identifier of P1.  The P1
** values need not be contiguous but all P1 values should be small integers.
** It is an error for P1 to be negative.
**
** If P5!=0 then use the content of register P2 as the root page, not
** the value of P2 itself.
**
** There will be a read lock on the database whenever there is an
** open cursor.  If the database was unlocked prior to this instruction
** then a read lock is acquired as part of this instruction.  A read
** lock allows other processes to read the database but prohibits
** any other process from modifying the database.  The read lock is
** released when all cursors are closed.  If this instruction attempts
** to get a read lock but fails, the script terminates with an
** SQLITE4_BUSY error code.
**
** The P4 value may be either an integer (P4_INT32) or a pointer to
** a KeyInfo structure (P4_KEYINFO). If it is a pointer to a KeyInfo 
** structure, then said structure defines the content and collating 
** sequence of the index being opened. Otherwise, if P4 is an integer 
** value, it is set to the number of columns in the table.
**
** See also OpenWrite.
*/
/* Opcode: OpenWrite P1 P2 P3 P4 P5
**
** Open a read/write cursor named P1 on the table or index whose root
** page is P2.  Or if P5!=0 use the content of register P2 to find the
** root page.
**
** The P4 value may be either an integer (P4_INT32) or a pointer to
** a KeyInfo structure (P4_KEYINFO). If it is a pointer to a KeyInfo 
** structure, then said structure defines the content and collating 
** sequence of the index being opened. Otherwise, if P4 is an integer 
** value, it is set to the number of columns in the table, or to the
** largest index of any column of the table that is actually used.
**
** This instruction works just like OpenRead except that it opens the cursor
** in read/write mode.  For a given table, there can be one or more read-only
** cursors or a single read/write cursor but not both.
**
** See also OpenRead.
*/
case OP_OpenRead:
case OP_OpenWrite: {
  int nField;
  KeyInfo *pKeyInfo;
  int p2;
  int iDb;
  KVStore *pX;
  VdbeCursor *pCur;
  Db *pDb;

  if( p->expired ){
    rc = SQLITE4_ABORT;
    break;
  }

  nField = 0;
  pKeyInfo = 0;
  p2 = pOp->p2;
  iDb = pOp->p3;
  assert( iDb>=0 && iDb<db->nDb );
  pDb = &db->aDb[iDb];
  pX = pDb->pKV;
  assert( pX!=0 );
  if( pOp->p5 ){
    assert( p2>0 );
    assert( p2<=p->nMem );
    pIn2 = &aMem[p2];
    assert( memIsValid(pIn2) );
    assert( (pIn2->flags & MEM_Int)!=0 );
    sqlite4VdbeMemIntegerify(pIn2);
    p2 = sqlite4_num_to_int32(pIn2->u.num, 0);
    /* The p2 value always comes from a prior OP_NewIdxid opcode and
    ** that opcode will always set the p2 value to 2 or more or else fail.
    ** If there were a failure, the prepared statement would have halted
    ** before reaching this instruction. */
    if( NEVER(p2<2) ) {
      rc = SQLITE4_CORRUPT_BKPT;
      goto abort_due_to_error;
    }
  }
  if( pOp->p4type==P4_KEYINFO ){
      printf("P4 is KEYINFO\n");
    pKeyInfo = pOp->p4.pKeyInfo;
    nField = pKeyInfo->nField+1;
    printf("pKeyInfo->nField: %d\n", pKeyInfo->nField);
    printf("pKeyInfo->nData: %d\n", pKeyInfo->nData);
    printf("pKeyInfo->nPK: %d\n", pKeyInfo->nPK);
  }else if( pOp->p4type==P4_INT32 ){
    nField = pOp->p4.i;
    printf("nField: %d\n", nField);
  }
  assert( pOp->p1>=0 );
  pCur = allocateCursor(p, pOp->p1, nField, iDb, 1);
  if( pCur==0 ) goto no_mem;
  pCur->nullRow = 1;
  pCur->iRoot = p2;
  printf("pCur->iRoot: %d\n", pCur->iRoot);
  rc = sqlite4KVStoreOpenCursor(pX, &pCur->pKVCur);
  pCur->pKeyInfo = pKeyInfo;
  break;
}

/* Opcode: OpenEphemeral P1 P2 * P4 P5
**
** Open a new cursor P1 to a transient table.
** The cursor is always opened read/write even if 
** the main database is read-only.  The ephemeral
** table is deleted automatically when the cursor is closed.
**
** P2 is the number of columns in the ephemeral table.
** The cursor points to a BTree table if P4==0 and to a BTree index
** if P4 is not 0.  If P4 is not NULL, it points to a KeyInfo structure
** that defines the format of keys in the index.
**
** This opcode was once called OpenTemp.  But that created
** confusion because the term "temp table", might refer either
** to a TEMP table at the SQL level, or to a table opened by
** this opcode.  Then this opcode was call OpenVirtual.  But
** that created confusion with the whole virtual-table idea.
**
** The P5 parameter can be a mask of the BTREE_* flags defined
** in btree.h.  These flags control aspects of the operation of
** the btree.  The BTREE_OMIT_JOURNAL and BTREE_SINGLE flags are
** added automatically.
*/
/* Opcode: OpenAutoindex P1 P2 * P4 *
**
** This opcode works the same as OP_OpenEphemeral.  It has a
** different name to distinguish its use.  Tables created using
** by this opcode will be used for automatically created transient
** indices in joins.
*/
case OP_OpenAutoindex: 
case OP_OpenEphemeral: {
  VdbeCursor *pCx;

  assert( pOp->p1>=0 );
  pCx = allocateCursor(p, pOp->p1, pOp->p2, -1, 1);
  if( pCx==0 ) goto no_mem;
  pCx->nullRow = 1;

  rc = sqlite4KVStoreOpen(db, "ephm", 0, &pCx->pTmpKV,
          SQLITE4_KVOPEN_TEMPORARY | SQLITE4_KVOPEN_NO_TRANSACTIONS
  );
  if( rc==SQLITE4_OK ) rc = sqlite4KVStoreOpenCursor(pCx->pTmpKV, &pCx->pKVCur);
  if( rc==SQLITE4_OK ) rc = sqlite4KVStoreBegin(pCx->pTmpKV, 2);

  pCx->pKeyInfo = pOp->p4.pKeyInfo;

  break;
}

/* Opcode: OpenSorter P1 P2 * P4 *
**
** This opcode works like OP_OpenEphemeral except that it opens
** a transient index that is specifically designed to sort large
** tables using an external merge-sort algorithm.
*/
case OP_SorterOpen: {
  /* VdbeCursor *pCx; */
  pOp->opcode = OP_OpenEphemeral;
  pc--;
  break;
}

/* Opcode: Close P1 * * * *
**
** Close a cursor previously opened as P1.  If P1 is not
** currently open, this instruction is a no-op.
*/
case OP_Close: {
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  sqlite4VdbeFreeCursor(p->apCsr[pOp->p1]);
  p->apCsr[pOp->p1] = 0;
  break;
}

/* Opcode: SeekPk P1 * P3 * *
**
** P1 must be a cursor open on a PRIMARY KEY index. P3 is a cursor open
** on an auxiliary index on the same table. P3 must be pointing to a valid
** index entry.
**
** This opcode seeks cursor P1 so that it points to the PK index entry
** that corresponds to the same table row as the current entry that 
** cursor P3 points to. The entry must exist.
**
** Actually, the seek is deferred until it is actually needed and if the
** PRIMARY KEY index is never referenced, the seek never takes place.  The
** sqlite3VdbeCursorMoveto() does the seek, if necessary.  If the target
** row does not exist in the PRIMARY KEY table, then the
** sqlite3VdbeCursorMoveto() routine will throw an SQLITE4_CORRUPT error.
*/
case OP_SeekPk: {
  KVByteArray *aKey;              /* Key data from cursor pIdx */
  KVSize nKey;                    /* Size of aKey[] in bytes */
  VdbeCursor *pPk;                /* Cursor P1 */
  VdbeCursor *pIdx;               /* Cursor P3 */
  int nShort;                     /* Size of aKey[] without PK fields */
  int nVarint;                    /* Size of varint pPk->iRoot */

  pPk = p->apCsr[pOp->p1];
  pIdx = p->apCsr[pOp->p3];

  if( pIdx->pFts ){
    rc = sqlite4Fts5Pk(pIdx->pFts, pPk->iRoot, &aKey, &nKey);
    if( rc==SQLITE4_OK ){
      rc = sqlite4KVCursorSeek(pPk->pKVCur, aKey, nKey, 0);
      if( rc==SQLITE4_NOTFOUND ) rc = SQLITE4_CORRUPT_BKPT;
      pPk->nullRow = 0;
    }
  }else{
    rc = sqlite4KVCursorKey(pIdx->pKVCur, (const KVByteArray **)&aKey, &nKey);
    if( rc!=SQLITE4_OK ) break;
    nShort = sqlite4VdbeShortKey(aKey, nKey, 
        pIdx->pKeyInfo->nField - pIdx->pKeyInfo->nPK, 0
    );
    nVarint = sqlite4VarintLen(pPk->iRoot);
    rc = sqlite4_buffer_resize(&pPk->sSeekKey, nVarint + nKey - nShort);
    if( rc!=SQLITE4_OK ) break;
    putVarint32((u8 *)(pPk->sSeekKey.p), pPk->iRoot);
    memcpy(((u8*)pPk->sSeekKey.p) + nVarint, &aKey[nShort], nKey-nShort);
    assert( pPk->sSeekKey.n>0 );
  }
  pPk->rowChnged = 1;

  break;
}

/* Opcode: SeekGe P1 P2 P3 P4 *
**
** P1 identifies an open database cursor. The cursor is repositioned so
** that it points to the smallest entry in its index that is greater than
** or equal to the key formed by the array of P4 registers starting at
** register P3.
**
** If there are no records greater than or equal to the key and P2 is 
** not zero, then jump to P2.
**
** See also: Found, NotFound, Distinct, SeekLt, SeekGt, SeekLe
*/
/* Opcode: SeekGt P1 P2 P3 P4 *
**
** P1 identifies an open database cursor. The cursor is repositioned so
** that it points to the smallest entry in its index that is greater than
** the key formed by the array of P4 registers starting at
** register P3.
**
** If there are no records greater than the key and P2 is 
** not zero, then jump to P2.
**
** See also: Found, NotFound, Distinct, SeekLt, SeekGe, SeekLe
*/
/* Opcode: SeekLt P1 P2 P3 P4 * 
**
** P1 identifies an open database cursor. The cursor is repositioned so
** that it points to the largest entry in its index that is less than
** the key formed by the array of P4 registers starting at
** register P3.
**
** If there are no records less than the key and P2 is 
** not zero, then jump to P2.
**
** See also: Found, NotFound, Distinct, SeekGt, SeekGe, SeekLe
*/
/* Opcode: SeekLe P1 P2 P3 P4 *
**
** P1 identifies an open database cursor. The cursor is repositioned so
** that it points to the largest entry in its index that is less than or
** equal to the key formed by the array of P4 registers starting at
** register P3.
**
** If there are no records less than or equal to the key and P2 is 
** not zero, then jump to P2.
**
** See also: Found, NotFound, Distinct, SeekGt, SeekGe, SeekLt
*/
case OP_SeekLt:         /* jump, in3 */
case OP_SeekLe:         /* jump, in3 */
case OP_SeekGe:         /* jump, in3 */
case OP_SeekGt: {       /* jump, in3 */
  int op;                         /* Copy of pOp->opcode (the op-code) */
  VdbeCursor *pC;                 /* Cursor P1 */
  int nField;                     /* Number of values to encode into key */
  KVByteArray *aProbe;            /* Buffer containing encoded key */
  KVSize nProbe;                  /* Size of aProbe[] in bytes */
  int dir;                        /* KV search dir (+ve or -ve) */
  const KVByteArray *aKey;        /* Pointer to final cursor key */
  KVSize nKey;                    /* Size of aKey[] in bytes */

  pC = p->apCsr[pOp->p1];
  pC->nullRow = 0;
  pC->sSeekKey.n = 0;
  pC->rowChnged = 1;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  assert( pOp->p2!=0 );
  assert( pC!=0 );
  assert( OP_SeekLe == OP_SeekLt+1 );
  assert( OP_SeekGe == OP_SeekLt+2 );
  assert( OP_SeekGt == OP_SeekLt+3 );

  dir = +1;
  op = pOp->opcode;
  if( op==OP_SeekLe || op==OP_SeekLt ) dir = -1;

  /* Encode a database key consisting of the contents of the P4 registers
  ** starting at register P3. Have the vdbecodec module allocate an extra
  ** free byte at the end of the database key (see below).  */
  nField = pOp->p4.i;
  pIn3 = &aMem[pOp->p3];
  if( pC->iRoot!=KVSTORE_ROOT ){
    rc = sqlite4VdbeEncodeKey(
        db, pIn3, nField, pC->iRoot, pC->pKeyInfo, &aProbe, &nProbe, 1
    );

    /*   Opcode    search-dir    increment-key
    **  --------------------------------------
    **   SeekLt    -1            no
    **   SeekLe    -1            yes
    **   SeekGe    +1            no
    **   SeekGt    +1            yes
    */
    if( op==OP_SeekLe || op==OP_SeekGt ) aProbe[nProbe++] = 0xFF;
    if( rc==SQLITE4_OK ){
      rc = sqlite4KVCursorSeek(pC->pKVCur, aProbe, nProbe, dir);
    }
  }else{
    Stringify(pIn3, encoding);
    rc = sqlite4KVCursorSeek(
        pC->pKVCur, (const KVByteArray *)pIn3->z, pIn3->n, dir
    );
  }

  if( rc==SQLITE4_OK ){
    if( op==OP_SeekLt ){
      rc = sqlite4KVCursorPrev(pC->pKVCur);
    }else if( op==OP_SeekGt ){
      rc = sqlite4KVCursorNext(pC->pKVCur);
    }
  }

  if( pC->iRoot!=KVSTORE_ROOT ){
    /* Check that the KV cursor currently points to an entry belonging
    ** to index pC->iRoot (and not an entry that is part of some other 
    ** index).  */
    if( rc==SQLITE4_OK || rc==SQLITE4_INEXACT ){
      rc = sqlite4KVCursorKey(pC->pKVCur, &aKey, &nKey);
      if( rc==SQLITE4_OK && memcmp(aKey, aProbe, sqlite4VarintLen(pC->iRoot)) ){
        rc = SQLITE4_NOTFOUND;
      }
    }

    /* Free the key allocated above. If no error has occurred but the cursor 
    ** does not currently point to a valid entry, jump to instruction P2.  */
    sqlite4DbFree(db, aProbe);
  }else if( rc==SQLITE4_INEXACT ){
    rc = SQLITE4_OK;
  }

#ifdef SQLITE4_TEST
  if( rc==SQLITE4_OK ){ sqlite4_search_count++; }
#endif
  if( rc==SQLITE4_NOTFOUND ){
    rc = SQLITE4_OK;
    pc = pOp->p2 - 1;
  }
  break;
}
 

/* Opcode: Found P1 P2 P3 P4 *
**
** If P4==0 then register P3 holds a blob constructed by MakeKey.  If
** P4>0 then register P3 is the first of P4 registers that should be
** combined to generate a key.
**
** Cursor P1 is open on an index.  If the record identified by P3 and P4
** is a prefix of any entry in P1 then a jump is made to P2 and
** P1 is left pointing at the matching entry.
*/
/* Opcode: NotFound P1 P2 P3 P4 *
**
** If P4==0 then register P3 holds a blob constructed by MakeKey.  If
** P4>0 then register P3 is the first of P4 registers that should be
** combined to generate key.
** 
** Cursor P1 is on an index.  If the record identified by P3 and P4
** is not the prefix of any entry in P1 then a jump is made to P2.  If P1 
** does contain an entry whose prefix matches the P3/P4 record then control
** falls through to the next instruction and P1 is left pointing at the
** matching entry.
**
** See also: Found, NotExists, IsUnique
*/
/* Opcode: NotExists P1 P2 P3 * *
**
** Use the content of register P3 as an integer key.  If a record 
** with that key does not exist in table of P1, then jump to P2. 
** If the record does exist, then fall through.  The cursor is left 
** pointing to the record if it exists.
**
** The difference between this operation and NotFound is that this
** operation assumes the key is an integer and that P1 is a table whereas
** NotFound assumes key is a blob constructed from MakeKey and
** P1 is an index.
**
** See also: Found, NotFound, IsUnique
*/
case OP_NotExists: {    /* jump, in3 */
  pOp->p4.i = 1;
  pOp->p4type = P4_INT32;
  /* Fall through into OP_NotFound */
}
case OP_NotFound:       /* jump, in3 */
case OP_Found: {        /* jump, in3 */
  int alreadyExists;
  VdbeCursor *pC;
  KVByteArray *pFree;
  KVByteArray *pProbe;
  KVSize nProbe;
  const KVByteArray *pKey;
  KVSize nKey;

#ifdef SQLITE4_TEST
  sqlite4_found_count++;
#endif

  alreadyExists = 0;
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  assert( pOp->p4type==P4_INT32 );
  pC = p->apCsr[pOp->p1];
  pC->sSeekKey.n = 0;
  pC->rowChnged = 1;
  assert( pC!=0 );
  pIn3 = &aMem[pOp->p3];
  assert( pC->pKVCur!=0 );
  if( pOp->p4.i>0 ){
    rc = sqlite4VdbeEncodeKey(
        db, pIn3, pOp->p4.i, pC->iRoot, pC->pKeyInfo, &pProbe, &nProbe, 0
    );
    pFree = pProbe;
  }else{
    pProbe = (KVByteArray*)pIn3->z;
    nProbe = pIn3->n;
    pFree = 0;
  }
  if( rc==SQLITE4_OK ){
    rc = sqlite4KVCursorSeek(pC->pKVCur, pProbe, nProbe, +1);
    if( rc==SQLITE4_INEXACT || rc==SQLITE4_OK ){
      rc = sqlite4KVCursorKey(pC->pKVCur, &pKey, &nKey);
      if( rc==SQLITE4_OK && nKey>=nProbe && memcmp(pKey, pProbe, nProbe)==0 ){
        alreadyExists = 1;
        pC->nullRow = 0;
      }
    }else if( rc==SQLITE4_NOTFOUND ){
      rc = SQLITE4_OK;
    }
  }
  sqlite4DbFree(db, pFree);
  if( pOp->opcode==OP_Found ){
    if( alreadyExists ) pc = pOp->p2 - 1;
  }else{
    if( !alreadyExists ) pc = pOp->p2 - 1;
  }
  break;
}

/* Opcode: IsUnique P1 P2 P3 P4 *
**
** Cursor P1 is open on an index that enforces a UNIQUE constraint. 
** Register P3 contains an encoded key suitable to be inserted into the 
** index. If the key can be inserted into the index without violating
** a UNIQUE constraint, jump to instruction P2. Otherwise, fall through
** to the next instruction.
**
** If P4 is a non-zero integer and the jump is not taken, then it is
** a register that currently contains a blob. At the start of the blob
** is a varint that contains the index number for the PRIMARY KEY index
** of the table. The contents of P4 are overwritten with an index key
** composed of the varint from the start of the initial blob content
** and the PRIMARY KEY values from the index entry causing the UNIQUE
** constraint to fail.
*/
case OP_IsUnique: {        /* jump, in3 */
  VdbeCursor *pC;
  Mem *pProbe;
  Mem *pOut;
  int iOut;
  int nShort;
  int dir;
  int bPk;
  u64 dummy;

  KVByteArray const *aKey;        /* Key read from cursor */
  KVSize nKey;                    /* Size of aKey in bytes */

  assert( pOp->p4type==P4_INT32 );

  pProbe = &aMem[pOp->p3];
  pC = p->apCsr[pOp->p1];
  pC->rowChnged = 1;
  pOut = (pOp->p4.i==0 ? 0 : &aMem[pOp->p4.i]);
  bPk = (pC->pKeyInfo->nPK==0);
  assert( pOut==0 || (pOut->flags & MEM_Blob) || bPk );

  if( bPk==0 ){
    nShort = sqlite4VdbeShortKey((u8 *)pProbe->z, pProbe->n, 
        pC->pKeyInfo->nField - pC->pKeyInfo->nPK, 0
        );
    assert( nShort<=pProbe->n );
    assert( (nShort==pProbe->n)==(pC->pKeyInfo->nPK==0) );
  }else{
    nShort = pProbe->n;
  }

  dir = !bPk;      /* "dir = (bPk ? 0 : 1);" */
  rc = sqlite4KVCursorSeek(pC->pKVCur, (u8 *)pProbe->z, nShort, dir);

  if( rc==SQLITE4_OK && pOut ){
    sqlite4VdbeMemCopy(pOut, pProbe);
  }else if( rc==SQLITE4_NOTFOUND ){
    rc = SQLITE4_OK;
    pc = pOp->p2-1;
  }else if( rc==SQLITE4_INEXACT ){
    assert( nShort<pProbe->n );
    assert( bPk==0 );
    rc = sqlite4KVCursorKey(pC->pKVCur, &aKey, &nKey);
    if( rc==SQLITE4_OK ){
      if( nKey<nShort || memcmp(pProbe->z, aKey, nShort) ){
        pc = pOp->p2-1;
      }else if( pOut ){
        iOut = sqlite4GetVarint64((u8 *)pOut->z, pOut->n, &dummy);
        rc = sqlite4VdbeMemGrow(pOut, iOut+(nKey - nShort), 1);
        if( rc==SQLITE4_OK ){
          memcpy(&pOut->z[iOut], &aKey[nShort], (nKey - nShort));
          pOut->n = iOut + (nKey - nShort);
        }
      }
    }
  }

  break;
}

/* Opcode: Sequence P1 P2 * * *
**
** Find the next available sequence number for cursor P1.
** Write the sequence number into register P2.
** The sequence number on the cursor is incremented after this
** instruction.  
*/
case OP_Sequence: {           /* out2-prerelease */
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  assert( p->apCsr[pOp->p1]!=0 );
  pOut->u.num = sqlite4_num_from_int64(p->apCsr[pOp->p1]->seqCount++);
  break;
}


/* Opcode: NewRowid P1 P2 P3 * *
**
** Get a new integer primary key (a.k.a "rowid") for table P1.  The integer
** should not be currently in use as a primary key on that table.
**
** If P3 is not zero, then it is the number of a register in the top-level
** frame that holds a lower bound for the new rowid.  In other words, the
** new rowid must be no less than reg[P3]+1.
*/
case OP_NewRowid: {           /* out2-prerelease */
  i64 v;                   /* The new rowid */
  VdbeCursor *pC;          /* Cursor of table to get the new rowid */
  const KVByteArray *aKey; /* Key of an existing row */
  KVSize nKey;             /* Size of the existing row key */
  int n;                   /* Number of bytes decoded */
  i64 i3;                  /* Integer value from pIn3 */
  sqlite4_num vNum;        /* Intermediate result */
  
  v = 0;
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );

  /* Some compilers complain about constants of the form 0x7fffffffffffffff.
  ** Others complain about 0x7ffffffffffffffffLL.  The following macro seems
  ** to provide the constant while making all compilers happy.
  */
# define MAX_ROWID  (i64)( (((u64)0x7fffffff)<<32) | (u64)0xffffffff )

  /* The next rowid or record number (different terms for the same
  ** thing) is obtained in a two-step algorithm.
  **
  ** First we attempt to find the largest existing rowid and add one
  ** to that.  But if the largest existing rowid is already the maximum
  ** positive integer, we have to fall through to the second
  ** probabilistic algorithm
  **
  ** The second algorithm is to select a rowid at random and see if
  ** it already exists in the table.  If it does not exist, we have
  ** succeeded.  If the random rowid does exist, we select a new one
  ** and try again, up to 100 times.
  */

  rc = sqlite4VdbeSeekEnd(pC, -2);
  if( rc==SQLITE4_NOTFOUND ){
    v = 0;
    rc = SQLITE4_OK;
  }else if( rc==SQLITE4_OK ){
    rc = sqlite4KVCursorKey(pC->pKVCur, &aKey, &nKey);
    if( rc==SQLITE4_OK ){
      n = sqlite4GetVarint64((u8 *)aKey, nKey, (u64 *)&v);
      printf("n: %d\n", n);
      printf("v: %d\n", v);
      if( n==0 ) rc = SQLITE4_CORRUPT_BKPT;
      if( v!=pC->iRoot ) rc = SQLITE4_CORRUPT_BKPT;
    }
    if( rc==SQLITE4_OK ){
      n = sqlite4VdbeDecodeNumericKey(&aKey[n], nKey-n, &vNum);
      printf("n: %d\n", n);
      printf("vNum->u.num->approx: %x\n",vNum.approx);
      printf("vNum->u.num->e: %x\n",vNum.e);
      printf("vNum->u.num->m: %d\n",vNum.m);
      printf("vNum->u.num->sign: %x\n",vNum.sign);
      if( n==0 || (v = sqlite4_num_to_int64(vNum,0))==LARGEST_INT64 ){
        assert( 0 );
        rc = SQLITE4_FULL;
      }
    }
  }else{
    break;
  }
#ifndef SQLITE_OMIT_AUTOINCREMENT
  if( pOp->p3 && rc==SQLITE4_OK ){
    pIn3 = sqlite4RegisterInRootFrame(p, pOp->p3);
    assert( memIsValid(pIn3) );
    REGISTER_TRACE(pOp->p3, pIn3);
    sqlite4VdbeMemIntegerify(pIn3);
    assert( (pIn3->flags & MEM_Int)!=0 );  /* mem(P3) holds an integer */
    i3 = sqlite4_num_to_int64(pIn3->u.num, 0);
    if( i3==MAX_ROWID ){
      rc = SQLITE4_FULL;
      assert( 0 );
    }
    if( v<i3 ) v = i3;
  }
#endif
  
  
  pOut->flags = MEM_Int;
  pOut->u.num = sqlite4_num_from_int64(v+1);
  printf("v: %d\n", v);
  printf("pOut->u.num->approx: %x\n",pOut->u.num.approx);
  printf("pOut->u.num->e: %x\n",pOut->u.num.e);
  printf("pOut->u.num->m: %d\n",pOut->u.num.m);
  printf("pOut->u.num->sign: %x\n",pOut->u.num.sign);
  
  break;
}

/* Opcode: NewIdxid P1 P2 * * *
**
** This opcode is used to allocated new integer index numbers. P1 must
** be an integer value when this opcode is invoked. Before the opcode
** concludes, P1 is set to a value 1 greater than the larger of:
**
**   * its current value, or 
**   * the largest index number still visible in the database using the 
**     LEFAST query mode used by OP_NewRowid in database P2.
*/
case OP_NewIdxid: {          /* fin1 */
  u64 iMax;
  i64 i1;
  KVStore *pKV;
  KVCursor *pCsr;
 
  pKV = db->aDb[pOp->p2].pKV;
  pIn1 = &aMem[pOp->p1];
  iMax = 0;
  assert( pIn1->flags & MEM_Int );

  rc = sqlite4KVStoreOpenCursor(pKV, &pCsr);
  if( rc==SQLITE4_OK ){
    const u8 aKey[] = { 0xFF, 0xFF };
    rc = sqlite4KVCursorSeek(pCsr, aKey, sizeof(aKey), -2);
    if( rc==SQLITE4_OK || rc==SQLITE4_INEXACT ){
      const KVByteArray *pKey;
      KVSize nKey;
      rc = sqlite4KVCursorKey(pCsr, &pKey, &nKey);
      if( rc==SQLITE4_OK ){
        sqlite4GetVarint64((const unsigned char *)pKey, nKey, &iMax);
      }
    }else if( rc==SQLITE4_NOTFOUND ){
      rc = SQLITE4_OK;
    }
    sqlite4KVCursorClose(pCsr);
  }

  i1 = sqlite4_num_to_int64(pIn1->u.num, 0);
  if( i1>=(i64)iMax ){
    i1++;
  }else{
    i1 = iMax+1;
  }
  pIn1->u.num = sqlite4_num_from_int64(i1);

  break;
}

/* Opcode: Delete P1 P2 * * *
**
** Delete the record at which the P1 cursor is currently pointing.
**
** The cursor will be left pointing at either the next or the previous
** record in the table. If it is left pointing at the next record, then
** the next Next instruction will be a no-op.  Hence it is OK to delete
** a record from within an Next loop.
**
** If the OPFLAG_NCHANGE flag of P2 is set, then the row change count is
** incremented (otherwise not).
**
** P1 must not be pseudo-table. It has to be a real table.
*/
case OP_Delete: {
  VdbeCursor *pC;
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  assert( pC->sSeekKey.n==0 );
  pC->rowChnged = 1;
  rc = sqlite4KVCursorDelete(pC->pKVCur);
  if( pOp->p2 & OPFLAG_NCHANGE ) p->nChange++;
  break;
}

/* Opcode: ResetCount * * * * *
**
** The value of the change counter is copied to the database handle
** change counter (returned by subsequent calls to sqlite4_changes()).
** Then the VMs internal change counter resets to 0.
** This is used by trigger programs.
*/
case OP_ResetCount: {
  sqlite4VdbeSetChanges(db, p->nChange);
  p->nChange = 0;
  break;
}

/* Opcode: GrpCompare P1 P2 P3
**
** P1 is a cursor used to sort records. Its keys consist of the fields being
** sorted on encoded as an ordinary database key, followed by a sequence 
** number encoded as defined by the comments surrounding OP_MakeIdxKey. 
** Register P3 either contains NULL, or such a key truncated so as to 
** remove the sequence number.
**
** This opcode compares the current key of P1, less the sequence number, 
** with the contents of register P3. If they are identical, jump to 
** instruction P2. Otherwise, replace the contents of P3 with the current
** key of P1 (minus the sequence number) and fall through to the next
** instruction.
*/
case OP_GrpCompare: {
  VdbeCursor *pC;                 /* Cursor P1 */
  KVByteArray const *aKey;        /* Key from cursor P1 */
  KVSize nKey;                    /* Size of aKey[] in bytes */

  pC = p->apCsr[pOp->p1];
  rc = sqlite4KVCursorKey(pC->pKVCur, &aKey, &nKey);
  if( rc==SQLITE4_OK ){
    for(nKey--; (aKey[nKey] & 0x80)==0; nKey--);
  }

  pIn3 = &aMem[pOp->p3];
  if( (pIn3->flags & MEM_Blob) 
   && pIn3->n==nKey && 0==memcmp(pIn3->z, aKey, nKey) 
  ){
    pc = pOp->p2-1;
  }else{
    sqlite4VdbeMemSetStr(pIn3, (const char*)aKey, nKey, 0, SQLITE4_TRANSIENT,0);
  }

  break;
};

/* Opcode: SorterData P1 P2 * * *
**
** Write into register P2 the current sorter data for sorter cursor P1.
*/
/* Opcode: RowData P1 P2 * * *
**
** Write into register P2 the complete row data for cursor P1.
** There is no interpretation of the data.  
** It is just copied onto the P2 register exactly as 
** it is found in the database file.
**
** If the P1 cursor must be pointing to a valid row (not a NULL row)
** of a real table, not a pseudo-table.
*/
/* Opcode: RowKey P1 P2 * * P5
**
** Write into register P2 the complete row key for cursor P1. There is 
** no interpretation of the data. The key is copied onto the P3 register 
** exactly as it is found in the database file.
**
** The P1 cursor must be pointing to a valid row (not a NULL row)
** of a real table, not a pseudo-table.
**
** If P5 is non-zero, it is a flag indicating that this value will be
** stored as a sample in the sqlite_stat3 table. At present, this means
** that the table number is stripped from the start of the record, and
** then all but the initial field removed from the end. In other words,
** the blob copied into register P2 is the first field of the index-key
** only.
*/
case OP_SorterData:
case OP_RowKey:
case OP_RowData: {
  VdbeCursor *pC;
  KVCursor *pCrsr;
  const KVByteArray *pData;
  KVSize nData;
  int nVarint;
  u64 dummy;

  pOut = &aMem[pOp->p2];
  memAboutToChange(p, pOut);

  /* Note that RowKey and RowData are really exactly the same instruction */
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  rc = sqlite4VdbeCursorMoveto(pC);
  if( rc!=SQLITE4_OK ) break;
  assert( pC->nullRow==0 );
  assert( pC->pKVCur!=0 );
  pCrsr = pC->pKVCur;

  if( pOp->opcode==OP_RowKey ){
    rc = sqlite4KVCursorKey(pCrsr, &pData, &nData);
    if( pOp->p5 ){
      nData = sqlite4VdbeShortKey(pData, nData, 1, 0);
      nVarint = sqlite4GetVarint64(pData, nData, &dummy);
      pData += nVarint;
      nData -= nVarint;
    }
  }else{
    rc = sqlite4KVCursorData(pCrsr, 0, -1, &pData, &nData);
  }
  if( rc==SQLITE4_OK && nData>db->aLimit[SQLITE4_LIMIT_LENGTH] ){
    goto too_big;
  }
  sqlite4VdbeMemSetStr(pOut, (const char*)pData, nData, 0, SQLITE4_TRANSIENT,0);
  pOut->enc = SQLITE4_UTF8;  /* In case the blob is ever cast to text */
  UPDATE_MAX_BLOBSIZE(pOut);
  break;
}

/* Opcode: AnalyzeKey P1 P2 P3 P4
**
** P1 is an open cursor that currently points to a valid row. P2 is a 
** register that contains either a NULL value, or an index key. If it is 
** not NULL, this opcode compares the key in register P2 with the key of 
** the row P1 currently points to and determines the number of fields in
** the prefix that the two keys share in common (which may be zero).
** Call this value N. If P2 is NULL, set N to P4.
**
** P3 is the first in an array of P4 registers containing integer values.
** The first N of these are left as is by this instruction. The remaining
** (P4-N) are incremented.
**
** Finally, the key belonging to the current row of cursor P1 is copied
** into register P2.
*/
case OP_AnalyzeKey: {
  VdbeCursor *pC;
  const KVByteArray *pNew;
  KVSize nNew;
  Mem *pKey;
  Mem *aIncr;
  int nEq;
  int nTotal;
  int i;

  pKey = &aMem[pOp->p2];
  aIncr = &aMem[pOp->p3];
  nTotal = pOp->p4.i;
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  assert( pC->nullRow==0 );
  assert( pC->pKVCur!=0 );
  assert( pOp->p4type==P4_INT32 );

  rc = sqlite4KVCursorKey(pC->pKVCur, &pNew, &nNew);
  if( rc==SQLITE4_OK ){
    assert( pKey->flags & (MEM_Blob|MEM_Null) );
    if( pKey->flags & MEM_Blob ){
      for(i=0; i<nNew && i<pKey->n && pNew[i]==(KVByteArray)pKey->z[i]; i++);

      /* The two keys share i bytes in common. Figure out how many fields
      ** this corresponds to. Store said value in variable nEq. */
      sqlite4VdbeShortKey(pNew, i, LARGEST_INT32, &nEq);
    }else{
      nEq = nTotal;
    }

    /* Increment nTotal-nEq registers */
    for(i=nEq; i<nTotal; i++){
      memAboutToChange(p, &aIncr[i]);
      sqlite4VdbeMemIntegerify(&aIncr[i]);
      aIncr[i].u.num = sqlite4_num_add(
          aIncr[i].u.num, sqlite4_num_from_int64(1)
      );
      REGISTER_TRACE(pOp->p1, &aIncr[i]);
    }

    /* Copy the new key into register P2 */
    memAboutToChange(p, pKey);
    sqlite4VdbeMemSetStr(pKey, (const char*)pNew, nNew, 0, SQLITE4_TRANSIENT,0);
    pKey->enc = SQLITE4_UTF8;
    UPDATE_MAX_BLOBSIZE(pKey);
  }

  break;
}

/* Opcode: Rowid P1 P2 * * *
**
** Store in register P2 an integer which is the key of the table entry that
** P1 is currently point to.
**
** P1 can be either an ordinary table or a virtual table.  There used to
** be a separate OP_VRowid opcode for use with virtual tables, but this
** one opcode now works for both table types.
*/
case OP_Rowid: {                 /* out2-prerelease */
  VdbeCursor *pC;
  i64 v;
  const KVByteArray *aKey;
  KVSize nKey;
  int n;
  sqlite4_num vNum;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  rc = sqlite4VdbeCursorMoveto(pC);
  if( rc!=SQLITE4_OK ) break;
  assert( pC->sSeekKey.n==0 );
  if( pC->nullRow ){
    pOut->flags = MEM_Null;
    break;
#ifndef SQLITE4_OMIT_VIRTUALTABLE
  }else if( pC->pVtabCursor ){
    pVtab = pC->pVtabCursor->pVtab;
    pModule = pVtab->pModule;
    assert( pModule->xRowid );
    rc = pModule->xRowid(pC->pVtabCursor, &v);
    importVtabErrMsg(p, pVtab);
#endif /* SQLITE4_OMIT_VIRTUALTABLE */
  }else{
    rc = sqlite4KVCursorKey(pC->pKVCur, &aKey, &nKey);
    if( rc==SQLITE4_OK ){
      n = sqlite4GetVarint64(aKey, nKey, (sqlite4_uint64*)&v);
      n = sqlite4VdbeDecodeNumericKey(&aKey[n], nKey-n, &vNum);
      if( n==0 ) rc = SQLITE4_CORRUPT;
      v = sqlite4_num_to_int64(vNum,0);
    }
  }
  pOut->u.num = sqlite4_num_from_int64(v);
  break;
}

/* Opcode: NullRow P1 * * * *
**
** Move the cursor P1 to a null row.  Any OP_Column operations
** that occur while the cursor is on the null row will always
** write a NULL.
*/
case OP_NullRow: {
  VdbeCursor *pC;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  pC->nullRow = 1;
  pC->rowChnged = 1;
  break;
}

/* Opcode: Last P1 P2 * * *
**
** The next use of the Rowid or Column or Next instruction for P1 
** will refer to the last entry in the database table or index.
** If the table or index is empty and P2>0, then jump immediately to P2.
** If P2 is 0 or if the table or index is not empty, fall through
** to the following instruction.
*/
case OP_Last: {        /* jump */
  VdbeCursor *pC;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  rc = sqlite4VdbeSeekEnd(pC, -1);
  if( rc==SQLITE4_NOTFOUND ){  
    rc = SQLITE4_OK;
    if( pOp->p2 ) pc = pOp->p2 - 1;
  }else{
    pC->nullRow = 0;
  }
  break;
}


/* Opcode: Sort P1 P2 * * *
**
** This opcode does exactly the same thing as OP_Rewind except that
** it increments an undocumented global variable used for testing.
**
** Sorting is accomplished by writing records into a sorting index,
** then rewinding that index and playing it back from beginning to
** end.  We use the OP_Sort opcode instead of OP_Rewind to do the
** rewinding so that the global variable will be incremented and
** regression tests can determine whether or not the optimizer is
** correctly optimizing out sorts.
*/
case OP_SorterSort:    /* jump */
  pOp->opcode = OP_Sort;
case OP_Sort: {        /* jump */
#ifdef SQLITE4_TEST
  sqlite4_sort_count++;
  sqlite4_search_count--;
#endif
  p->aCounter[SQLITE4_STMTSTATUS_SORT-1]++;
  /* Fall through into OP_Rewind */
}
/* Opcode: Rewind P1 P2 * * *
**
** The next use of the Rowid or Column or Next instruction for P1 
** will refer to the first entry in the database table or index.
** If the table or index is empty and P2>0, then jump immediately to P2.
** If P2 is 0 or if the table or index is not empty, fall through
** to the following instruction.
*/
case OP_Rewind: {        /* jump */
  VdbeCursor *pC;
  int doJump;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  pC = p->apCsr[pOp->p1];
  assert( pC!=0 );
  doJump = 1;
  rc = sqlite4VdbeSeekEnd(pC, +1);
  if( rc==SQLITE4_NOTFOUND ){
    rc = SQLITE4_OK;
    doJump = 1;
  }else{
    doJump = 0;
  }
  pC->nullRow = (u8)doJump;
  assert( pOp->p2>0 && pOp->p2<p->nOp );
  if( doJump ){
    pc = pOp->p2 - 1;
  }
  break;
}

/* Opcode: Next P1 P2 * P4 P5
**
** Advance cursor P1 so that it points to the next key/data pair in its
** table or index.  If there are no more key/value pairs then fall through
** to the following instruction.  But if the cursor advance was successful,
** jump immediately to P2.
**
** The P1 cursor must be for a real table, not a pseudo-table.
**
** P4 is always of type P4_ADVANCE. The function pointer points to
** sqlite4VdbeNext().
**
** If P5 is positive and the jump is taken, then event counter
** number P5-1 in the prepared statement is incremented.
**
** See also: Prev
*/
/* Opcode: Prev P1 P2 * * P5
**
** Back up cursor P1 so that it points to the previous key/data pair in its
** table or index.  If there is no previous key/value pairs then fall through
** to the following instruction.  But if the cursor backup was successful,
** jump immediately to P2.
**
** The P1 cursor must be for a real table, not a pseudo-table.
**
** P4 is always of type P4_ADVANCE. The function pointer points to
** sqlite4VdbePrevious().
**
** If P5 is positive and the jump is taken, then event counter
** number P5-1 in the prepared statement is incremented.
*/
case OP_SorterNext:    /* jump */
  pOp->opcode = OP_Next;
case OP_Prev:          /* jump */
case OP_Next: {        /* jump */
  VdbeCursor *pC;

  CHECK_FOR_INTERRUPT;
  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  assert( pOp->p5<=ArraySize(p->aCounter) );
  pC = p->apCsr[pOp->p1];
  if( pC==0 ){
    break;  /* See ticket #2273 */
  }
  assert( pOp->opcode!=OP_Next || pOp->p4.xAdvance==sqlite4VdbeNext );
  assert( pOp->opcode!=OP_Prev || pOp->p4.xAdvance==sqlite4VdbePrevious );
  rc = pOp->p4.xAdvance(pC);
  if( rc==SQLITE4_OK ){
    pc = pOp->p2 - 1;
    if( pOp->p5 ) p->aCounter[pOp->p5-1]++;
    pC->nullRow = 0;
#ifdef SQLITE4_TEST
    sqlite4_search_count++;
#endif
  }else if( rc==SQLITE4_NOTFOUND ){
    pC->nullRow = 1;
    rc = SQLITE4_OK;
  }
  break;
}


/* Opcode: Insert P1 P2 P3 * P5
**
** Register P3 holds the key and register P2 holds the data for an
** index entry.  Write this record into the index specified by the
** cursor P1.
**
** P3 can be either an integer or a blob.  If it is a blob then its value
** is used as-is as the KVStore key.  If P3 is an integer, then the KVStore
** key is constructed using P3 as the INTEGER PRIMARY KEY value.
**
** If the OPFLAG_NCHANGE flag of P5 is set, then the row change count is
** incremented (otherwise not).
*/
case OP_Insert: {
  VdbeCursor *pC;
  Mem *pKey;
  Mem *pData;
  int nKVKey;
  KVByteArray *pKVKey;
  KVByteArray aKey[24];
  

  pC = p->apCsr[pOp->p1];
  pKey = &aMem[pOp->p3];
  pData = pOp->p2 ? &aMem[pOp->p2] : 0;

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  assert( pC && pC->pKVCur && pC->pKVCur->pStore );
  assert( pData==0 || (pData->flags & MEM_Blob) );

  if( pOp->p5 & OPFLAG_NCHANGE ) p->nChange++;

  if( pKey->flags & MEM_Int ){
    nKVKey = sqlite4PutVarint64(aKey, pC->iRoot);
    nKVKey += sqlite4VdbeEncodeIntKey(aKey+nKVKey, sqlite4VdbeIntValue(pKey));
    pKVKey = aKey;
  }else{
    nKVKey = pKey->n;
    pKVKey = pKey->z;
  }


  rc = sqlite4KVStoreReplace(
     pC->pKVCur->pStore,
     (u8 *)pKVKey, nKVKey,
     (u8 *)(pData ? pData->z : 0), (pData ? pData->n : 0)
  );
  pC->rowChnged = 1;

  break;
}

/* Opcode: IdxDelete P1 * P3 * *
**
** P1 is a cursor open on a database index. P3 contains a key suitable for
** the index. Delete P3 from P1 if it is present.
*/
case OP_IdxDelete: {
  VdbeCursor *pC;
  Mem *pKey;

  pC = p->apCsr[pOp->p1];
  pKey = &aMem[pOp->p3];

  assert( pOp->p1>=0 && pOp->p1<p->nCursor );
  assert( pC && pC->pKVCur && pC->pKVCur->pStore );
  assert( pKey->flags & MEM_Blob );

  rc = sqlite4KVCursorSeek(pC->pKVCur, (u8 *)pKey->z, pKey->n, 0);
  if( rc==SQLITE4_OK ){
    rc = sqlite4KVCursorDelete(pC->pKVCur);
  }else if( rc==SQLITE4_NOTFOUND ){
    rc = SQLITE4_OK;
  }
  pC->rowChnged = 1;

  break;
}

/* Opcode: IdxRowkey P1 P2 P3 * *
**
** Cursor P1 points to an index entry. Extract the encoded primary key 
** fields from the entry. Then set output register P2 to a blob value
** containing the value P3 as a varint followed by the encoded PK
** fields.
**
** See also: Rowkey
*/
case OP_IdxRowkey: {              /* out2-prerelease */
  KVByteArray const *aKey;        /* Key data from cursor pIdx */
  KVSize nKey;                    /* Size of aKey[] in bytes */
  int nShort;                     /* Size of aKey[] without PK fields */
  KVByteArray *aPkKey;            /* Pointer to PK buffer */
  KVSize nPkKey;                  /* Size of aPkKey in bytes */
  int iRoot;
  VdbeCursor *pC;

  iRoot = pOp->p3;
  pOut = &aMem[pOp->p2];
  memAboutToChange(p, pOut);

  pC = p->apCsr[pOp->p1];

  rc = sqlite4KVCursorKey(pC->pKVCur, &aKey, &nKey);
  if( rc!=SQLITE4_OK ) break;

  nShort = sqlite4VdbeShortKey(aKey, nKey, 
      pC->pKeyInfo->nField - pC->pKeyInfo->nPK, 0
  );

  nPkKey = sqlite4VarintLen(iRoot) + nKey - nShort;
  if( nPkKey>db->aLimit[SQLITE4_LIMIT_LENGTH] ){
    goto too_big;
  }

  rc = sqlite4VdbeMemGrow(pOut, nPkKey, 0);
  if( rc!=SQLITE4_OK ) break;
  aPkKey = pOut->z;
  putVarint32(aPkKey, iRoot);
  memcpy(&aPkKey[nPkKey - (nKey-nShort)], &aKey[nShort], nKey-nShort);
  pOut->type = SQLITE4_BLOB;
  pOut->n = nPkKey;
  MemSetTypeFlag(pOut, MEM_Blob);

  pOut->enc = SQLITE4_UTF8;  /* In case the blob is ever cast to text */
  UPDATE_MAX_BLOBSIZE(pOut);
  break;
}

/* Opcode: IdxGE P1 P2 P3
**
** P1 is an open cursor. P3 contains a database key formatted by MakeKey.
** This opcode compares the current key that index P1 points to with
** the key in register P3.
**
** If the index key is greater than or equal to the key in register P3, 
** then jump to instruction P2. Otherwise, fall through to the next VM
** instruction. The comparison is done using memcmp(), except that if P3
** is a prefix of the P1 key they are considered equal.
*/
case OP_IdxLT:          /* jump */
case OP_IdxLE:          /* jump */
case OP_IdxGE:          /* jump */
case OP_IdxGT: {        /* jump */
  VdbeCursor *pC;                 /* Cursor P1 */
  KVByteArray const *aKey;        /* Key from cursor P1 */
  KVSize nKey;                    /* Size of aKey[] in bytes */
  Mem *pCmp;                      /* Memory cell to compare index key with */
  int nCmp;                       /* Bytes of data to compare using memcmp() */
  int res;                        /* Result of memcmp() call */
  int bJump;                      /* True to take the jump */

  pCmp = &aMem[pOp->p3];
  assert( pCmp->flags & MEM_Blob );
  pC = p->apCsr[pOp->p1];
  rc = sqlite4KVCursorKey(pC->pKVCur, &aKey, &nKey);

  if( rc==SQLITE4_OK ){
    nCmp = pCmp->n;
    if( nCmp>nKey ) nCmp = nKey;

    res = memcmp(aKey, pCmp->z, nCmp);
    switch( pOp->opcode ){
      case OP_IdxLT: bJump = (res <  0); break;
      case OP_IdxLE: bJump = (res <= 0); break;
      case OP_IdxGE: bJump = (res >= 0); break;
      case OP_IdxGT: bJump = (res >  0); break;
    }

    if( bJump ) pc = pOp->p2 - 1;
  }
  break;
}

/* Opcode: Clear P1 P2 * * P5
**
** Delete all contents of the database table or index whose table number
** in the database file is given by P1.  
**
** The table being clear is in the main database file if P2==0.  If
** P2==1 then the table to be clear is in the auxiliary database file
** that is used to store tables create using CREATE TEMPORARY TABLE.
**
** If the OPFLAG_NCHANGE flag of P5 is set, then the row change count is
** incremented (otherwise not).
**
** See also: Destroy
*/
case OP_Clear: {
  KVCursor *pCur;
  KVByteArray const *aKey;
  KVSize nKey;
  KVSize nProbe;
  KVByteArray aProbe[12];

  nProbe = sqlite4PutVarint64(aProbe, pOp->p1);
  rc = sqlite4KVStoreOpenCursor(db->aDb[pOp->p2].pKV, &pCur);
  if( rc ) break;
  rc = sqlite4KVCursorSeek(pCur, aProbe, nProbe, +1);
  while( rc!=SQLITE4_NOTFOUND ){
    if( pOp->p5 & OPFLAG_NCHANGE ) p->nChange++;
    rc = sqlite4KVCursorKey(pCur, &aKey, &nKey);
    if( rc!=SQLITE4_OK ) break;
    if( nKey<nProbe ){ rc = SQLITE4_CORRUPT; break; }
    if( memcmp(aKey, aProbe, nProbe)!=0 ) break;
    rc = sqlite4KVCursorDelete(pCur);
    if( rc ) break;
    rc = sqlite4KVCursorNext(pCur);
  }
  sqlite4KVCursorClose(pCur);
  if( rc==SQLITE4_NOTFOUND) rc = SQLITE4_OK;
  break;
}


/* Opcode: ParseSchema P1 * * P4 *
**
** Read and parse all entries from the SQLITE4_MASTER table of database P1
** that match the WHERE clause P4. 
**
** This opcode invokes the parser to create a new virtual machine,
** then runs the new virtual machine.  It is thus a re-entrant opcode.
*/
case OP_ParseSchema: {
  int iDb;
  const char *zMaster;
  char *zSql;
  InitData initData;

  iDb = pOp->p1;
  assert( iDb>=0 && iDb<db->nDb );
  assert( DbHasProperty(db, iDb, DB_SchemaLoaded) );
  /* Used to be a conditional */ {
    zMaster = SCHEMA_TABLE(iDb);
    initData.db = db;
    initData.iDb = pOp->p1;
    initData.pzErrMsg = &p->zErrMsg;
    zSql = sqlite4MPrintf(db,
       "SELECT name, rootpage, sql FROM '%q'.%s WHERE %s ORDER BY rowid",
       db->aDb[iDb].zName, zMaster, pOp->p4.z);
    if( zSql==0 ){
      rc = SQLITE4_NOMEM;
    }else{
      assert( db->init.busy==0 );
      db->init.busy = 1;
      initData.rc = SQLITE4_OK;
      assert( !db->mallocFailed );
      rc = sqlite4_exec(db, zSql, sqlite4InitCallback, &initData);
      if( rc==SQLITE4_OK ) rc = initData.rc;
      sqlite4DbFree(db, zSql);
      db->init.busy = 0;
    }
  }
  if( rc==SQLITE4_NOMEM ){
    goto no_mem;
  }
  break;  
}

#if !defined(SQLITE4_OMIT_ANALYZE)
/* Opcode: LoadAnalysis P1 * * * *
**
** Read the sqlite_stat1 table for database P1 and load the content
** of that table into the internal index hash table.  This will cause
** the analysis to be used when preparing all subsequent queries.
*/
case OP_LoadAnalysis: {
  assert( pOp->p1>=0 && pOp->p1<db->nDb );
  rc = sqlite4AnalysisLoad(db, pOp->p1);
  break;  
}
#endif /* !defined(SQLITE4_OMIT_ANALYZE) */

/* Opcode: DropTable P1 * * P4 *
**
** Remove the internal (in-memory) data structures that describe
** the table named P4 in database P1.  This is called after a table
** is dropped in order to keep the internal representation of the
** schema consistent with what is on disk.
*/
case OP_DropTable: {
  sqlite4UnlinkAndDeleteTable(db, pOp->p1, pOp->p4.z);
  break;
}

/* Opcode: DropIndex P1 * * P4 *
**
** Remove the internal (in-memory) data structures that describe
** the index named P4 in database P1.  This is called after an index
** is dropped in order to keep the internal representation of the
** schema consistent with what is on disk.
*/
case OP_DropIndex: {
  sqlite4UnlinkAndDeleteIndex(db, pOp->p1, pOp->p4.z);
  break;
}

/* Opcode: DropTrigger P1 * * P4 *
**
** Remove the internal (in-memory) data structures that describe
** the trigger named P4 in database P1.  This is called after a trigger
** is dropped in order to keep the internal representation of the
** schema consistent with what is on disk.
*/
case OP_DropTrigger: {
  sqlite4UnlinkAndDeleteTrigger(db, pOp->p1, pOp->p4.z);
  break;
}

/* Opcode: RowSetTest P1 P2 P3 * *
**
** Register P1 contains a RowSet object. Register P3 contains a database 
** key. This function checks if the RowSet already contains an equal key.
** If so, control jumps to instruction P2. Otherwise, fall through to the
** next instruction.
**
** TODO: Optimization similar to SQLite 3 using P4.
*/
case OP_RowSetTest: {        /* in1, in3, jump */
  int iSet;
  pIn1 = &aMem[pOp->p1];
  pIn3 = &aMem[pOp->p3];
  iSet = pOp->p4.i;

  if( 0!=(pIn1->flags & MEM_RowSet)
   && 0!=sqlite4RowSetTest(pIn1->u.pRowSet, iSet, (u8 *)pIn3->z, pIn3->n)
  ){
    pc = pOp->p2-1;
    break;
  }

  /* Fall through to RowSetAdd */
}

/* Opcode: RowSetAdd P1 P2 * * *
**
** Read the blob value from register P2 and store it in RowSet object P1.
*/
case OP_RowSetAdd: {         /* in1, in3 */
  pIn1 = &aMem[pOp->p1];
  if( (pIn1->flags & MEM_RowSet)==0 ){
    sqlite4VdbeMemSetRowSet(pIn1);
    if( (pIn1->flags & MEM_RowSet)==0 ) goto no_mem;
  }
  pIn3 = &aMem[pOp->p3];
  assert( pIn3->flags & MEM_Blob );
  sqlite4RowSetInsert(pIn1->u.pRowSet, (u8 *)pIn3->z, pIn3->n);
  break;
}

/* Opcode: RowSetRead P1 P2 P3 * *
**
** Remove a value from MemSet object P1 and store it in register P3.
** Or, if MemSet P1 is already empty, leave P3 unchanged and jump to 
** instruction P2.
*/
case OP_RowSetRead: {       /* in1 */
  const u8 *aKey;
  int nKey;

  CHECK_FOR_INTERRUPT;
  pIn1 = &aMem[pOp->p1];
  pOut = &aMem[pOp->p3];
  if( (pIn1->flags & MEM_RowSet)
   && (aKey = sqlite4RowSetRead(pIn1->u.pRowSet, &nKey))
  ){
    rc = sqlite4VdbeMemSetStr(pOut, (char const *)aKey, nKey, 0,
                              SQLITE4_TRANSIENT, 0);
    sqlite4RowSetNext(pIn1->u.pRowSet);
  }else{
    /* The RowSet is empty */
    sqlite4VdbeMemSetNull(pIn1);
    pc = pOp->p2 - 1;
  }

  break;
}

#ifndef SQLITE4_OMIT_TRIGGER

/* Opcode: Program P1 P2 P3 P4 *
**
** Execute the trigger program passed as P4 (type P4_SUBPROGRAM). 
**
** P1 contains the address of the memory cell that contains the first memory 
** cell in an array of values used as arguments to the sub-program. P2 
** contains the address to jump to if the sub-program throws an IGNORE 
** exception using the RAISE() function. Register P3 contains the address 
** of a memory cell in this (the parent) VM that is used to allocate the 
** memory required by the sub-vdbe at runtime.
**
** P4 is a pointer to the VM containing the trigger program.
*/
case OP_Program: {        /* jump */
  int nMem;               /* Number of memory registers for sub-program */
  int nByte;              /* Bytes of runtime space required for sub-program */
  Mem *pRt;               /* Register to allocate runtime space */
  Mem *pMem;              /* Used to iterate through memory cells */
  Mem *pEnd;              /* Last memory cell in new array */
  VdbeFrame *pFrame;      /* New vdbe frame to execute in */
  SubProgram *pProgram;   /* Sub-program to execute */

  pProgram = pOp->p4.pProgram;
  pRt = &aMem[pOp->p3];
  assert( pProgram->nOp>0 );

  if( p->nFrame>=db->aLimit[SQLITE4_LIMIT_TRIGGER_DEPTH] ){
    rc = SQLITE4_ERROR;
    sqlite4SetString(&p->zErrMsg, db, "too many levels of trigger recursion");
    break;
  }

  /* Register pRt is used to store the memory required to save the state
  ** of the current program, and the memory required at runtime to execute
  ** the trigger program. If this trigger has been fired before, then pRt 
  ** is already allocated. Otherwise, it must be initialized.  */
  if( (pRt->flags&MEM_Frame)==0 ){
    /* SubProgram.nMem is set to the number of memory cells used by the 
    ** program stored in SubProgram.aOp. As well as these, one memory
    ** cell is required for each cursor used by the program. Set local
    ** variable nMem (and later, VdbeFrame.nChildMem) to this value.
    */
    nMem = pProgram->nMem + pProgram->nCsr;
    nByte = ROUND8(sizeof(VdbeFrame))
              + nMem * sizeof(Mem)
              + pProgram->nCsr * sizeof(VdbeCursor *)
              + pProgram->nOnce * sizeof(u8);
    pFrame = sqlite4DbMallocZero(db, nByte);
    if( !pFrame ){
      goto no_mem;
    }
    sqlite4VdbeMemRelease(pRt);
    pRt->flags = MEM_Frame;
    pRt->u.pFrame = pFrame;

    pFrame->v = p;
    pFrame->nChildMem = nMem;
    pFrame->nChildCsr = pProgram->nCsr;
    pFrame->pc = pc;
    pFrame->aMem = p->aMem;
    pFrame->nMem = p->nMem;
    pFrame->apCsr = p->apCsr;
    pFrame->nCursor = p->nCursor;
    pFrame->aOp = p->aOp;
    pFrame->nOp = p->nOp;
    pFrame->token = pProgram->token;
    pFrame->aOnceFlag = p->aOnceFlag;
    pFrame->nOnceFlag = p->nOnceFlag;

    pEnd = &VdbeFrameMem(pFrame)[pFrame->nChildMem];
    for(pMem=VdbeFrameMem(pFrame); pMem!=pEnd; pMem++){
      pMem->flags = MEM_Invalid;
      pMem->db = db;
    }
  }else{
    pFrame = pRt->u.pFrame;
    assert( pProgram->nMem+pProgram->nCsr==pFrame->nChildMem );
    assert( pProgram->nCsr==pFrame->nChildCsr );
    assert( pc==pFrame->pc );
  }

  p->nFrame++;
  pFrame->pParent = p->pFrame;
  pFrame->nChange = p->nChange;
  p->nChange = 0;
  p->pFrame = pFrame;
  p->aMem = aMem = &VdbeFrameMem(pFrame)[-1];
  p->nMem = pFrame->nChildMem;
  p->nCursor = (u16)pFrame->nChildCsr;
  p->apCsr = (VdbeCursor **)&aMem[p->nMem+1];
  p->aOp = aOp = pProgram->aOp;
  p->nOp = pProgram->nOp;
  p->aOnceFlag = (u8 *)&p->apCsr[p->nCursor];
  p->nOnceFlag = pProgram->nOnce;
  pc = -1;
  memset(p->aOnceFlag, 0, p->nOnceFlag);

  break;
}

/* Opcode: Param P1 P2 * * *
**
** This opcode is only ever present in sub-programs called via the 
** OP_Program instruction. Copy a value currently stored in a memory 
** cell of the calling (parent) frame to cell P2 in the current frames 
** address space. This is used by trigger programs to access the new.* 
** and old.* values.
**
** The address of the cell in the parent frame is determined by adding
** the value of the P1 argument to the value of the P1 argument to the
** calling OP_Program instruction.
*/
case OP_Param: {           /* out2-prerelease */
  VdbeFrame *pFrame;
  Mem *pIn;
  pFrame = p->pFrame;
  pIn = &pFrame->aMem[pOp->p1 + pFrame->aOp[pFrame->pc].p1];   
  assert( memIsValid(pIn) );
  sqlite4VdbeMemShallowCopy(pOut, pIn, MEM_Ephem);
  break;
}

#endif /* #ifndef SQLITE4_OMIT_TRIGGER */

#ifndef SQLITE4_OMIT_FOREIGN_KEY
/* Opcode: FkCounter P1 P2 * * *
**
** Increment a "constraint counter" by P2 (P2 may be negative or positive).
** If P1 is non-zero, the database constraint counter is incremented 
** (deferred foreign key constraints). Otherwise, if P1 is zero, the 
** statement counter is incremented (immediate foreign key constraints).
*/
case OP_FkCounter: {
  if( pOp->p1 ){
    db->nDeferredCons += pOp->p2;
  }else{
    p->nFkConstraint += pOp->p2;
  }
  break;
}

/* Opcode: FkIfZero P1 P2 * * *
**
** This opcode tests if a foreign key constraint-counter is currently zero.
** If so, jump to instruction P2. Otherwise, fall through to the next 
** instruction.
**
** If P1 is non-zero, then the jump is taken if the database constraint-counter
** is zero (the one that counts deferred constraint violations). If P1 is
** zero, the jump is taken if the statement constraint-counter is zero
** (immediate foreign key constraint violations).
*/
case OP_FkIfZero: {         /* jump */
  if( pOp->p1 ){
    if( db->nDeferredCons==0 ) pc = pOp->p2-1;
  }else{
    if( p->nFkConstraint==0 ) pc = pOp->p2-1;
  }
  break;
}
#endif /* #ifndef SQLITE4_OMIT_FOREIGN_KEY */

#ifndef SQLITE4_OMIT_AUTOINCREMENT
/* Opcode: MemMax P1 P2 * * *
**
** P1 is a register in the root frame of this VM (the root frame is
** different from the current frame if this instruction is being executed
** within a sub-program). Set the value of register P1 to the maximum of 
** its current value and the value in register P2.
**
** This instruction throws an error if the memory cell is not initially
** an integer.
*/
case OP_MemMax: {        /* in2 */
  i64 i1;
  i64 i2;
  Mem *pIn1;
  pIn1 = sqlite4RegisterInRootFrame(p, pOp->p1);
  assert( memIsValid(pIn1) );
  sqlite4VdbeMemIntegerify(pIn1);
  pIn2 = &aMem[pOp->p2];
  REGISTER_TRACE(pOp->p1, pIn1);
  sqlite4VdbeMemIntegerify(pIn2);
  i1 = sqlite4_num_to_int64(pIn1->u.num, 0);
  i2 = sqlite4_num_to_int64(pIn2->u.num, 0);
  if( i1<i2 ){
    pIn1->u.num = sqlite4_num_from_int64(i2);
  }
  REGISTER_TRACE(pOp->p1, pIn1);
  break;
}
#endif /* SQLITE4_OMIT_AUTOINCREMENT */

/* Opcode: IfPos P1 P2 * * *
**
** If the value of register P1 is 1 or greater, jump to P2.
**
** It is illegal to use this instruction on a register that does
** not contain an integer.  An assertion fault will result if you try.
*/
case OP_IfPos: {        /* jump, in1 */
  i64 i1;
  pIn1 = &aMem[pOp->p1];
  assert( pIn1->flags&MEM_Int );
  i1 = sqlite4_num_to_int64(pIn1->u.num, 0);
  if( i1>0 ){
     pc = pOp->p2 - 1;
  }
  break;
}

/* Opcode: IfNeg P1 P2 * * *
**
** If the value of register P1 is less than zero, jump to P2. 
**
** It is illegal to use this instruction on a register that does
** not contain an integer.  An assertion fault will result if you try.
*/
case OP_IfNeg: {        /* jump, in1 */
  i64 i1;
  pIn1 = &aMem[pOp->p1];
  assert( pIn1->flags&MEM_Int );
  i1 = sqlite4_num_to_int64(pIn1->u.num, 0);
  if( i1<0 ){
     pc = pOp->p2 - 1;
  }
  break;
}

/* Opcode: IfZero P1 P2 P3 * *
**
** The register P1 must contain an integer.  Add literal P3 to the
** value in register P1.  If the result is exactly 0, jump to P2. 
**
** It is illegal to use this instruction on a register that does
** not contain an integer.  An assertion fault will result if you try.
*/
case OP_IfZero: {        /* jump, in1 */
  i64 i1;
  pIn1 = &aMem[pOp->p1];
  assert( pIn1->flags&MEM_Int );
  i1 = sqlite4_num_to_int64(pIn1->u.num, 0);
  i1 += pOp->p3;
  pIn1->u.num = sqlite4_num_from_int64(i1);
  if( i1==0 ){
     pc = pOp->p2 - 1;
  }
  break;
}

/* Opcode: AggStep * P2 P3 P4 P5
**
** Execute the step function for an aggregate.  The
** function has P5 arguments.   P4 is a pointer to the FuncDef
** structure that specifies the function.  Use register
** P3 as the accumulator.
**
** The P5 arguments are taken from register P2 and its
** successors.
*/
case OP_AggStep: {
  int n;
  int i;
  Mem *pMem;
  Mem *pRec;
  sqlite4_context ctx;
  sqlite4_value **apVal;

  n = pOp->p5;
  assert( n>=0 );
  pRec = &aMem[pOp->p2];
  apVal = p->apArg;
  assert( apVal || n==0 );
  for(i=0; i<n; i++, pRec++){
    assert( memIsValid(pRec) );
    apVal[i] = pRec;
    memAboutToChange(p, pRec);
    sqlite4VdbeMemStoreType(pRec);
  }
  ctx.pFunc = pOp->p4.pFunc;
  assert( pOp->p3>0 && pOp->p3<=p->nMem );
  ctx.pMem = pMem = &aMem[pOp->p3];
  pMem->n++;
  ctx.s.flags = MEM_Null;
  ctx.s.z = 0;
  ctx.s.zMalloc = 0;
  ctx.s.xDel = 0;
  ctx.s.db = db;
  ctx.isError = 0;
  ctx.pColl = 0;
  if( ctx.pFunc->flags & SQLITE4_FUNC_NEEDCOLL ){
    assert( pOp>p->aOp );
    assert( pOp[-1].p4type==P4_COLLSEQ );
    assert( pOp[-1].opcode==OP_CollSeq );
    ctx.pColl = pOp[-1].p4.pColl;
  }
  (ctx.pFunc->xStep)(&ctx, n, apVal); /* IMP: R-24505-23230 */
  if( ctx.isError ){
    sqlite4SetString(&p->zErrMsg, db, "%s", 
        (const char *)sqlite4ValueText(&ctx.s, SQLITE4_UTF8)
    );
    rc = ctx.isError;
  }

  sqlite4VdbeMemRelease(&ctx.s);

  break;
}

/* Opcode: AggFinal P1 P2 * P4 *
**
** Execute the finalizer function for an aggregate.  P1 is
** the memory location that is the accumulator for the aggregate.
**
** P2 is the number of arguments that the step function takes and
** P4 is a pointer to the FuncDef for this function.  The P2
** argument is not used by this opcode.  It is only there to disambiguate
** functions that can take varying numbers of arguments.  The
** P4 argument is only needed for the degenerate case where
** the step function was not previously called.
*/
case OP_AggFinal: {
  Mem *pMem;
  assert( pOp->p1>0 && pOp->p1<=p->nMem );
  pMem = &aMem[pOp->p1];
  assert( (pMem->flags & ~(MEM_Null|MEM_Agg))==0 );
  rc = sqlite4VdbeMemFinalize(pMem, pOp->p4.pFunc);
  if( rc ){
    sqlite4SetString(&p->zErrMsg, db, "%s", 
        (const char *)sqlite4ValueText(pMem, SQLITE4_UTF8)
    );
  }
  sqlite4VdbeChangeEncoding(pMem, encoding);
  UPDATE_MAX_BLOBSIZE(pMem);
  if( sqlite4VdbeMemTooBig(pMem) ){
    goto too_big;
  }
  break;
}

#ifndef SQLITE4_OMIT_PRAGMA
/* Opcode: JournalMode P1 P2 P3 * P5
**
** Change the journal mode of database P1 to P3. P3 must be one of the
** PAGER_JOURNALMODE_XXX values. If changing between the various rollback
** modes (delete, truncate, persist, off and memory), this is a simple
** operation. No IO is required.
**
** If changing into or out of WAL mode the procedure is more complicated.
**
** Write a string containing the final journal-mode to register P2.
*/
case OP_JournalMode: {    /* out2-prerelease */
  break;
};
#endif /* SQLITE4_OMIT_PRAGMA */


/* Opcode: Expire P1 * * * *
**
** Cause precompiled statements to become expired. An expired statement
** fails with an error code of SQLITE4_SCHEMA if it is ever executed 
** (via sqlite4_step()).
** 
** If P1 is 0, then all SQL statements become expired. If P1 is non-zero,
** then only the currently executing statement is affected. 
*/
case OP_Expire: {
  if( !pOp->p1 ){
    sqlite4ExpirePreparedStatements(db);
  }else{
    p->expired = 1;
  }
  break;
}


#ifndef SQLITE4_OMIT_VIRTUALTABLE
/* Opcode: VBegin * * * P4 *
**
** P4 may be a pointer to an sqlite4_vtab structure. If so, call the 
** xBegin method for that table.
**
** Also, whether or not P4 is set, check that this is not being called from
** within a callback to a virtual table xSync() method. If it is, the error
** code will be set to SQLITE4_LOCKED.
*/
case OP_VBegin: {
  VTable *pVTab;
  pVTab = pOp->p4.pVtab;
  rc = sqlite4VtabBegin(db, pVTab);
  if( pVTab ) importVtabErrMsg(p, pVTab->pVtab);
  break;
}
#endif /* SQLITE4_OMIT_VIRTUALTABLE */

#ifndef SQLITE4_OMIT_VIRTUALTABLE
/* Opcode: VCreate P1 * * P4 *
**
** P4 is the name of a virtual table in database P1. Call the xCreate method
** for that table.
*/
case OP_VCreate: {
  rc = sqlite4VtabCallCreate(db, pOp->p1, pOp->p4.z, &p->zErrMsg);
  break;
}
#endif /* SQLITE4_OMIT_VIRTUALTABLE */

#ifndef SQLITE4_OMIT_VIRTUALTABLE
/* Opcode: VDestroy P1 * * P4 *
**
** P4 is the name of a virtual table in database P1.  Call the xDestroy method
** of that table.
*/
case OP_VDestroy: {
  p->inVtabMethod = 2;
  rc = sqlite4VtabCallDestroy(db, pOp->p1, pOp->p4.z);
  p->inVtabMethod = 0;
  break;
}
#endif /* SQLITE4_OMIT_VIRTUALTABLE */

#ifndef SQLITE4_OMIT_VIRTUALTABLE
/* Opcode: VOpen P1 * * P4 *
**
** P4 is a pointer to a virtual table object, an sqlite4_vtab structure.
** P1 is a cursor number.  This opcode opens a cursor to the virtual
** table and stores that cursor in P1.
*/
case OP_VOpen: {
  VdbeCursor *pCur;
  sqlite4_vtab_cursor *pVtabCursor;
  sqlite4_vtab *pVtab;
  sqlite4_module *pModule;

  pCur = 0;
  pVtabCursor = 0;
  pVtab = pOp->p4.pVtab->pVtab;
  pModule = (sqlite4_module *)pVtab->pModule;
  assert(pVtab && pModule);
  rc = pModule->xOpen(pVtab, &pVtabCursor);
  importVtabErrMsg(p, pVtab);
  if( SQLITE4_OK==rc ){
    /* Initialize sqlite4_vtab_cursor base class */
    pVtabCursor->pVtab = pVtab;

    /* Initialise vdbe cursor object */
    pCur = allocateCursor(p, pOp->p1, 0, -1, 0);
    if( pCur ){
      pCur->pVtabCursor = pVtabCursor;
      pCur->pModule = pVtabCursor->pVtab->pModule;
    }else{
      db->mallocFailed = 1;
      pModule->xClose(pVtabCursor);
    }
  }
  break;
}
#endif /* SQLITE4_OMIT_VIRTUALTABLE */

#ifndef SQLITE4_OMIT_VIRTUALTABLE
/* Opcode: VFilter P1 P2 P3 P4 *
**
** P1 is a cursor opened using VOpen.  P2 is an address to jump to if
** the filtered result set is empty.
**
** P4 is either NULL or a string that was generated by the xBestIndex
** method of the module.  The interpretation of the P4 string is left
** to the module implementation.
**
** This opcode invokes the xFilter method on the virtual table specified
** by P1.  The integer query plan parameter to xFilter is stored in register
** P3. Register P3+1 stores the argc parameter to be passed to the
** xFilter method. Registers P3+2..P3+1+argc are the argc
** additional parameters which are passed to
** xFilter as argv. Register P3+2 becomes argv[0] when passed to xFilter.
**
** A jump is made to P2 if the result set after filtering would be empty.
*/
case OP_VFilter: {   /* jump */
  int nArg;
  int iQuery;
  const sqlite4_module *pModule;
  Mem *pQuery;
  Mem *pArgc;
  sqlite4_vtab_cursor *pVtabCursor;
  sqlite4_vtab *pVtab;
  VdbeCursor *pCur;
  int res;
  int i;
  Mem **apArg;

  pQuery = &aMem[pOp->p3];
  pArgc = &pQuery[1];
  pCur = p->apCsr[pOp->p1];
  assert( memIsValid(pQuery) );
  REGISTER_TRACE(pOp->p3, pQuery);
  assert( pCur->pVtabCursor );
  pVtabCursor = pCur->pVtabCursor;
  pVtab = pVtabCursor->pVtab;
  pModule = pVtab->pModule;

  /* Grab the index number and argc parameters */
  assert( (pQuery->flags&MEM_Int)!=0 && pArgc->flags==MEM_Int );
  nArg = (int)pArgc->u.i;
  iQuery = (int)pQuery->u.i;

  /* Invoke the xFilter method */
  {
    res = 0;
    apArg = p->apArg;
    for(i = 0; i<nArg; i++){
      apArg[i] = &pArgc[i+1];
      sqlite4VdbeMemStoreType(apArg[i]);
    }

    p->inVtabMethod = 1;
    rc = pModule->xFilter(pVtabCursor, iQuery, pOp->p4.z, nArg, apArg);
    p->inVtabMethod = 0;
    importVtabErrMsg(p, pVtab);
    if( rc==SQLITE4_OK ){
      res = pModule->xEof(pVtabCursor);
    }

    if( res ){
      pc = pOp->p2 - 1;
    }
  }
  pCur->nullRow = 0;

  break;
}
#endif /* SQLITE4_OMIT_VIRTUALTABLE */

#ifndef SQLITE4_OMIT_VIRTUALTABLE
/* Opcode: VColumn P1 P2 P3 * *
**
** Store the value of the P2-th column of
** the row of the virtual-table that the 
** P1 cursor is pointing to into register P3.
*/
case OP_VColumn: {
  sqlite4_vtab *pVtab;
  const sqlite4_module *pModule;
  Mem *pDest;
  sqlite4_context sContext;

  VdbeCursor *pCur = p->apCsr[pOp->p1];
  assert( pCur->pVtabCursor );
  assert( pOp->p3>0 && pOp->p3<=p->nMem );
  pDest = &aMem[pOp->p3];
  memAboutToChange(p, pDest);
  if( pCur->nullRow ){
    sqlite4VdbeMemSetNull(pDest);
    break;
  }
  pVtab = pCur->pVtabCursor->pVtab;
  pModule = pVtab->pModule;
  assert( pModule->xColumn );
  memset(&sContext, 0, sizeof(sContext));

  /* The output cell may already have a buffer allocated. Move
  ** the current contents to sContext.s so in case the user-function 
  ** can use the already allocated buffer instead of allocating a 
  ** new one.
  */
  sqlite4VdbeMemMove(&sContext.s, pDest);
  MemSetTypeFlag(&sContext.s, MEM_Null);

  rc = pModule->xColumn(pCur->pVtabCursor, &sContext, pOp->p2);
  importVtabErrMsg(p, pVtab);
  if( sContext.isError ){
    rc = sContext.isError;
  }

  /* Copy the result of the function to the P3 register. We
  ** do this regardless of whether or not an error occurred to ensure any
  ** dynamic allocation in sContext.s (a Mem struct) is  released.
  */
  sqlite4VdbeChangeEncoding(&sContext.s, encoding);
  sqlite4VdbeMemMove(pDest, &sContext.s);
  REGISTER_TRACE(pOp->p3, pDest);
  UPDATE_MAX_BLOBSIZE(pDest);

  if( sqlite4VdbeMemTooBig(pDest) ){
    goto too_big;
  }
  break;
}
#endif /* SQLITE4_OMIT_VIRTUALTABLE */

#ifndef SQLITE4_OMIT_VIRTUALTABLE
/* Opcode: VNext P1 P2 * * *
**
** Advance virtual table P1 to the next row in its result set and
** jump to instruction P2.  Or, if the virtual table has reached
** the end of its result set, then fall through to the next instruction.
*/
case OP_VNext: {   /* jump */
  sqlite4_vtab *pVtab;
  const sqlite4_module *pModule;
  int res;
  VdbeCursor *pCur;

  res = 0;
  pCur = p->apCsr[pOp->p1];
  assert( pCur->pVtabCursor );
  if( pCur->nullRow ){
    break;
  }
  pVtab = pCur->pVtabCursor->pVtab;
  pModule = pVtab->pModule;
  assert( pModule->xNext );

  /* Invoke the xNext() method of the module. There is no way for the
  ** underlying implementation to return an error if one occurs during
  ** xNext(). Instead, if an error occurs, true is returned (indicating that 
  ** data is available) and the error code returned when xColumn or
  ** some other method is next invoked on the save virtual table cursor.
  */
  p->inVtabMethod = 1;
  rc = pModule->xNext(pCur->pVtabCursor);
  p->inVtabMethod = 0;
  importVtabErrMsg(p, pVtab);
  if( rc==SQLITE4_OK ){
    res = pModule->xEof(pCur->pVtabCursor);
  }

  if( !res ){
    /* If there is data, jump to P2 */
    pc = pOp->p2 - 1;
  }
  break;
}
#endif /* SQLITE4_OMIT_VIRTUALTABLE */

#ifndef SQLITE4_OMIT_VIRTUALTABLE
/* Opcode: VRename P1 * * P4 *
**
** P4 is a pointer to a virtual table object, an sqlite4_vtab structure.
** This opcode invokes the corresponding xRename method. The value
** in register P1 is passed as the zName argument to the xRename method.
*/
case OP_VRename: {
  sqlite4_vtab *pVtab;
  Mem *pName;

  pVtab = pOp->p4.pVtab->pVtab;
  pName = &aMem[pOp->p1];
  assert( pVtab->pModule->xRename );
  assert( memIsValid(pName) );
  REGISTER_TRACE(pOp->p1, pName);
  assert( pName->flags & MEM_Str );
  testcase( pName->enc==SQLITE4_UTF8 );
  testcase( pName->enc==SQLITE4_UTF16BE );
  testcase( pName->enc==SQLITE4_UTF16LE );
  rc = sqlite4VdbeChangeEncoding(pName, SQLITE4_UTF8);
  if( rc==SQLITE4_OK ){
    rc = pVtab->pModule->xRename(pVtab, pName->z);
    importVtabErrMsg(p, pVtab);
    p->expired = 0;
  }
  break;
}
#endif

#ifndef SQLITE4_OMIT_VIRTUALTABLE
/* Opcode: VUpdate P1 P2 P3 P4 *
**
** P4 is a pointer to a virtual table object, an sqlite4_vtab structure.
** This opcode invokes the corresponding xUpdate method. P2 values
** are contiguous memory cells starting at P3 to pass to the xUpdate 
** invocation. The value in register (P3+P2-1) corresponds to the 
** p2th element of the argv array passed to xUpdate.
**
** The xUpdate method will do a DELETE or an INSERT or both.
** The argv[0] element (which corresponds to memory cell P3)
** is the rowid of a row to delete.  If argv[0] is NULL then no 
** deletion occurs.  The argv[1] element is the rowid of the new 
** row.  This can be NULL to have the virtual table select the new 
** rowid for itself.  The subsequent elements in the array are 
** the values of columns in the new row.
**
** If P2==1 then no insert is performed.  argv[0] is the rowid of
** a row to delete.
**
** P1 is a boolean flag. If it is set to true and the xUpdate call
** is successful, then the value returned by sqlite4_last_insert_rowid() 
** is set to the value of the rowid for the row just inserted.
*/
case OP_VUpdate: {
  sqlite4_vtab *pVtab;
  sqlite4_module *pModule;
  int nArg;
  int i;
  sqlite4_int64 rowid;
  Mem **apArg;
  Mem *pX;

  assert( pOp->p2==1        || pOp->p5==OE_Fail   || pOp->p5==OE_Rollback 
       || pOp->p5==OE_Abort || pOp->p5==OE_Ignore || pOp->p5==OE_Replace
  );
  pVtab = pOp->p4.pVtab->pVtab;
  pModule = (sqlite4_module *)pVtab->pModule;
  nArg = pOp->p2;
  assert( pOp->p4type==P4_VTAB );
  if( ALWAYS(pModule->xUpdate) ){
    u8 vtabOnConflict = db->vtabOnConflict;
    apArg = p->apArg;
    pX = &aMem[pOp->p3];
    for(i=0; i<nArg; i++){
      assert( memIsValid(pX) );
      memAboutToChange(p, pX);
      sqlite4VdbeMemStoreType(pX);
      apArg[i] = pX;
      pX++;
    }
    db->vtabOnConflict = pOp->p5;
    rc = pModule->xUpdate(pVtab, nArg, apArg, &rowid);
    db->vtabOnConflict = vtabOnConflict;
    importVtabErrMsg(p, pVtab);
    if( rc==SQLITE4_CONSTRAINT && pOp->p4.pVtab->bConstraint ){
      if( pOp->p5==OE_Ignore ){
        rc = SQLITE4_OK;
      }else{
        p->errorAction = ((pOp->p5==OE_Replace) ? OE_Abort : pOp->p5);
      }
    }else{
      p->nChange++;
    }
  }
  break;
}
#endif /* SQLITE4_OMIT_VIRTUALTABLE */

#ifndef SQLITE4_OMIT_TRACE
/* Opcode: Trace * * * P4 *
**
** If tracing is enabled (by the sqlite4_trace()) interface, then
** the UTF-8 string contained in P4 is emitted on the trace callback.
*/
case OP_Trace: {
  char *zTrace;
  char *z;

  if( db->xTrace && (zTrace = (pOp->p4.z ? pOp->p4.z : p->zSql))!=0 ){
    z = sqlite4VdbeExpandSql(p, zTrace);
    db->xTrace(db->pTraceArg, z);
    sqlite4DbFree(db, z);
  }
#ifdef SQLITE4_DEBUG
  if( (db->flags & SQLITE4_SqlTrace)!=0
   && (zTrace = (pOp->p4.z ? pOp->p4.z : p->zSql))!=0
  ){
    sqlite4DebugPrintf("SQL-trace: %s\n", zTrace);
  }
#endif /* SQLITE4_DEBUG */
  break;
}
#endif

/* Opcode: FtsUpdate P1 P2 P3 P4 P5
**
** This opcode is used to write to an FTS index. P4 points to an Fts5Info 
** object describing the index.
**
** If argument P5 is non-zero, then entries are removed from the FTS index.
** If it is zero, then entries are inserted. In other words, when a row
** is deleted from a table with an FTS index, this opcode is invoked with
** P5==1. When a row is inserted, it is invoked with P5==0. If an existing
** row is updated, this opcode is invoked twice - once with P5==1 and then
** again with P5==0.
**
** Register P1 contains the PK (a blob in key format) of the affected row.
** P3 is the first in an array of N registers, where N is the number of
** columns in the indexed table. Each register contains the value for the
** corresponding table column.
**
** If P2 is non-zero, then it is a register containing the root page number
** of the fts index to update. If it is zero, then the root page of the 
** index is available as part of the Fts5Info structure.
*/
case OP_FtsUpdate: {
  Fts5Info *pInfo;                /* Description of fts5 index to update */
  Mem *pKey;                      /* Primary key of indexed row */
  Mem *aArg;                      /* Pointer to array of N arguments */
  int iRoot;                      /* Root page number (or 0) */

  assert( pOp->p4type==P4_FTS5INFO );
  pInfo = pOp->p4.pFtsInfo;
  aArg = &aMem[pOp->p3];
  pKey = &aMem[pOp->p1];

  if( pOp->p2 ){
    iRoot = sqlite4_num_to_int32(aMem[pOp->p2].u.num, 0);
  }else{
    iRoot = 0;
  }

  rc = sqlite4Fts5Update(db, pInfo, iRoot, pKey, aArg, pOp->p5, &p->zErrMsg);
  break;
}

/*
** Opcode: FtsCksum P1 * P3 P4 P5
**
** This opcode is used by the integrity-check procedure that verifies that
** the contents of an fts5 index and its corresponding table match.
*/
case OP_FtsCksum: {
  Fts5Info *pInfo;                /* Description of fts5 index to update */
  Mem *pKey;                      /* Primary key of row */
  Mem *aArg;                      /* Pointer to array of N values */
  i64 cksum;                      /* Checksum for this row or index entry */
  i64 i1;

  assert( pOp->p4type==P4_FTS5INFO );
  pInfo = pOp->p4.pFtsInfo;

  pOut = &aMem[pOp->p1];
  pKey = &aMem[pOp->p3];
  aArg = &aMem[pOp->p3+1];
  cksum = 0;

  if( pOp->p5 ){
    sqlite4Fts5EntryCksum(db, pInfo, pKey, aArg, &cksum);
  }else{
    sqlite4Fts5RowCksum(db, pInfo, pKey, aArg, &cksum);
  }
  i1 = sqlite4_num_to_int64(pOut->u.num, 0);
  pOut->u.num = sqlite4_num_from_int64(i1 ^ cksum);

  break;
}

/* Opcode: FtsOpen P1 P2 P3 P4 P5
**
** Open an FTS cursor named P1. P4 points to an Fts5Info object.
**
** Register P3 contains the MATCH expression that this cursor will iterate
** through the matches for. P5 is set to 0 to iterate through the results
** in ascending PK order, or 1 for descending PK order.
**
** If the expression matches zero rows, jump to instruction P2. Otherwise,
** leave the cursor pointing at the first match and fall through to the
** next instruction.
*/
case OP_FtsOpen: {          /* jump */
  Fts5Info *pInfo;                /* Description of fts5 index to update */
  VdbeCursor *pCur;
  char *zMatch;
  Mem *pMatch;

  pMatch = &aMem[pOp->p3];
  Stringify(pMatch, encoding);
  zMatch = pMatch->z;

  assert( pOp->p4type==P4_FTS5INFO );
  pInfo = pOp->p4.pFtsInfo;
  pCur = allocateCursor(p, pOp->p1, 0, pInfo->iDb, 0);
  if( pCur ){
    rc = sqlite4Fts5Open(db, pInfo, zMatch, pOp->p5, &pCur->pFts, &p->zErrMsg);
  }
  if( rc==SQLITE4_OK && 0==sqlite4Fts5Valid(pCur->pFts) ){
    pc = pOp->p2-1;
  }
  break;
}

/* Opcode: FtsNext P1 P2 * * *
**
** Advance FTS cursor P1 to the next entry and jump to instruction P2. Or,
** if there is no next entry, set the cursor to point to EOF and fall through
** to the next instruction.
*/
case OP_FtsNext: {
  VdbeCursor *pCsr;

  pCsr = p->apCsr[pOp->p1];
  rc = sqlite4Fts5Next(pCsr->pFts);
  if( rc==SQLITE4_OK && sqlite4Fts5Valid(pCsr->pFts) ) pc = pOp->p2-1;

  break;
}

/* Opcode: FtsPk P1 P2 * * * 
**
** P1 is an FTS cursor that points to a valid entry (not EOF). Copy the PK 
** blob for the current entry to register P2.
*/
case OP_FtsPk: {
  assert( 0 );
  break;
}

/* Opcode: Noop * * * * *
**
** Do nothing.  This instruction is often useful as a jump
** destination.
*/
/*
** The magic Explain opcode are only inserted when explain==2 (which
** is to say when the EXPLAIN QUERY PLAN syntax is used.)
** This opcode records information from the optimizer.  It is the
** the same as a no-op.  This opcode never appears in a real VM program.
*/
default: {          /* This is really OP_Noop and OP_Explain */
  assert( pOp->opcode==OP_Noop || pOp->opcode==OP_Explain );
  break;
}

/*****************************************************************************
** The cases of the switch statement above this line should all be indented
** by 6 spaces.  But the left-most 6 spaces have been removed to improve the
** readability.  From this point on down, the normal indentation rules are
** restored.
*****************************************************************************/
    }

#ifdef VDBE_PROFILE
    {
      u64 elapsed = sqlite4Hwtime() - start;
      pOp->cycles += elapsed;
      pOp->cnt++;
#if 0
        fprintf(stdout, "%10llu ", elapsed);
        sqlite4VdbePrintOp(stdout, origPc, &aOp[origPc]);
#endif
    }
#endif

    /* The following code adds nothing to the actual functionality
    ** of the program.  It is only here for testing and debugging.
    ** On the other hand, it does burn CPU cycles every time through
    ** the evaluator loop.  So we can leave it out when NDEBUG is defined.
    */
#ifndef NDEBUG
    assert( pc>=-1 && pc<p->nOp );

#ifdef SQLITE4_DEBUG
    if( p->trace ){
      if( rc!=0 ) fprintf(p->trace,"rc=%d\n",rc);
      if( pOp->opflags & (OPFLG_OUT2_PRERELEASE|OPFLG_OUT2) ){
        registerTrace(p->trace, pOp->p2, &aMem[pOp->p2]);
      }
      if( pOp->opflags & OPFLG_OUT3 ){
        registerTrace(p->trace, pOp->p3, &aMem[pOp->p3]);
      }
    }
#endif  /* SQLITE4_DEBUG */
#endif  /* NDEBUG */
  }  /* The end of the for(;;) loop the loops through opcodes */

  /* If we reach this point, it means that execution is finished with
  ** an error of some kind.
  */
vdbe_error_halt:
  assert( rc );
  p->rc = rc;
  testcase( sqlite4DefaultEnv.xLog!=0 );
  sqlite4_log(db->pEnv, rc, "statement aborts at %d: [%s] %s", 
                   pc, p->zSql, p->zErrMsg);
  sqlite4VdbeHalt(p);
  if( rc==SQLITE4_IOERR_NOMEM ) db->mallocFailed = 1;
  rc = SQLITE4_ERROR;
  if( resetSchemaOnFault>0 ){
    sqlite4ResetInternalSchema(db, resetSchemaOnFault-1);
  }

  /* This is the only way out of this procedure.  We have to
  ** release the mutexes on btrees that were acquired at the
  ** top. */
vdbe_return:
  return rc;

  /* Jump to here if a string or blob larger than SQLITE4_MAX_LENGTH
  ** is encountered.
  */
too_big:
  sqlite4SetString(&p->zErrMsg, db, "string or blob too big");
  rc = SQLITE4_TOOBIG;
  goto vdbe_error_halt;

  /* Jump to here if a malloc() fails.
  */
no_mem:
  db->mallocFailed = 1;
  sqlite4SetString(&p->zErrMsg, db, "out of memory");
  rc = SQLITE4_NOMEM;
  goto vdbe_error_halt;

  /* Jump to here for any other kind of fatal error.  The "rc" variable
  ** should hold the error number.
  */
abort_due_to_error:
  assert( p->zErrMsg==0 );
  if( db->mallocFailed ) rc = SQLITE4_NOMEM;
  if( rc!=SQLITE4_IOERR_NOMEM ){
    sqlite4SetString(&p->zErrMsg, db, "%s", sqlite4ErrStr(rc));
  }
  goto vdbe_error_halt;

  /* Jump to here if the sqlite4_interrupt() API sets the interrupt
  ** flag.
  */
abort_due_to_interrupt:
  assert( db->u1.isInterrupted );
  rc = SQLITE4_INTERRUPT;
  p->rc = rc;
  sqlite4SetString(&p->zErrMsg, db, "%s", sqlite4ErrStr(rc));
  goto vdbe_error_halt;
}
