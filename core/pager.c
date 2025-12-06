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
** This is the implementation of the page cache subsystem or "pager".
**
** The pager is used to access a database disk file.  It implements
** atomic commit and rollback through the use of a journal file that
** is separate from the database file.  The pager also implements file
** locking to prevent two processes from writing the same database
** file simultaneously, or one process from reading the database while
** another is writing.
**
** @(#) $Id: pager.c,v 1.46 2002/05/30 12:27:03 drh Exp $
**
** 2001年9月15日
**
** 作者放弃此源代码的版权。作为法律声明的替代，这里有一个祝福：
**
**    愿你行善而不作恶。
**    愿你宽恕自己并宽恕他人。
**    愿你自由分享，索取不超所予。
**
** *************************************************************************
** 这是页面缓存子系统或“分页器”的实现。
**
** 分页器用于访问数据库磁盘文件。它通过使用与数据库文件分离的日志文件
** 来实现原子提交和回滚。分页器还实现了文件锁定，以防止两个进程
** 同时写入同一个数据库文件，或一个进程在另一个进程写入时读取数据库。
*/
#include "pager.h"
#include "os.h"
#include "sqlite.h"
#include "sqliteInt.h"
#include <assert.h>
#include <string.h>

/*
** The page cache as a whole is always in one of the following
** states:
**
**   SQLITE_UNLOCK       The page cache is not currently reading or
**                       writing the database file.  There is no
**                       data held in memory.  This is the initial
**                       state.
**
**   SQLITE_READLOCK     The page cache is reading the database.
**                       Writing is not permitted.  There can be
**                       multiple readers accessing the same database
**                       file at the same time.
**
**   SQLITE_WRITELOCK    The page cache is writing the database.
**                       Access is exclusive.  No other processes or
**                       threads can be reading or writing while one
**                       process is writing.
**
** The page cache comes up in SQLITE_UNLOCK.  The first time a
** sqlite_page_get() occurs, the state transitions to SQLITE_READLOCK.
** After all pages have been released using sqlite_page_unref(),
** the state transitions back to SQLITE_UNLOCK.  The first time
** that sqlite_page_write() is called, the state transitions to
** SQLITE_WRITELOCK.  (Note that sqlite_page_write() can only be
** called on an outstanding page which means that the pager must
** be in SQLITE_READLOCK before it transitions to SQLITE_WRITELOCK.)
** The sqlite_page_rollback() and sqlite_page_commit() functions
** transition the state from SQLITE_WRITELOCK back to SQLITE_READLOCK.
**
** 页面缓存作为一个整体始终处于以下状态之一：
**
**   SQLITE_UNLOCK       页面缓存当前未读取或写入数据库文件。
**                       内存中没有保存任何数据。这是初始状态。
**
**   SQLITE_READLOCK     页面缓存正在读取数据库。不允许写入。
**                       可以有多个读取器同时访问同一个数据库文件。
**
**   SQLITE_WRITELOCK    页面缓存正在写入数据库。访问是独占的。
** 当一个进程正在写入时，没有其他进程或线程可以读取或写入。
**
** 页面缓存启动时处于SQLITE_UNLOCK状态。第一次发生sqlite_page_get()时，
** 状态转换为SQLITE_READLOCK。在使用sqlite_page_unref()释放所有页面后，
** 状态转换回SQLITE_UNLOCK。第一次调用sqlite_page_write()时，
** 状态转换为SQLITE_WRITELOCK。（请注意，sqlite_page_write()只能在
** 未完成的页面上调用，这意味着分页器在转换为SQLITE_WRITELOCK之前必须
** 处于SQLITE_READLOCK状态。）sqlite_page_rollback()和sqlite_page_commit()
** 函数将状态从SQLITE_WRITELOCK转换回SQLITE_READLOCK。
*/
#define SQLITE_UNLOCK 0
#define SQLITE_READLOCK 1
#define SQLITE_WRITELOCK 2

/*
** Each in-memory image of a page begins with the following header.
** This header is only visible to this pager module.  The client
** code that calls pager sees only the data that follows the header.
**
** 页面的每个内存映像都以以下标头开头。
** 此标头仅对此分页器模块可见。调用分页器的客户端代码仅看到标头后面的数据。
*/
typedef struct PgHdr PgHdr;
struct PgHdr {
  Pager *pPager;                /* The pager to which this page belongs */
                                /* 此页面所属的分页器 */
  Pgno pgno;                    /* The page number for this page */
                                /* 此页面的页码 */
  PgHdr *pNextHash, *pPrevHash; /* Hash collision chain for PgHdr.pgno */
                                /* PgHdr.pgno 的哈希冲突链 */
  int nRef;                     /* Number of users of this page */
                                /* 此页面的用户数量 */
  PgHdr *pNextFree, *pPrevFree; /* Freelist of pages where nRef==0 */
                                /* nRef==0 的页面空闲列表 */
  PgHdr *pNextAll, *pPrevAll;   /* A list of all pages */
                                /* 所有页面的列表 */
  char inJournal;               /* TRUE if has been written to journal */
                                /* 如果已写入日志，则为 TRUE */
  char inCkpt;                  /* TRUE if written to the checkpoint journal */
                                /* 如果已写入检查点日志，则为 TRUE */
  char dirty;                   /* TRUE if we need to write back changes */
                                /* 如果我们需要写回更改，则为 TRUE */
  /* SQLITE_PAGE_SIZE bytes of page data follow this header */
  /* 此标头后面是 SQLITE_PAGE_SIZE 字节的页面数据 */
  /* Pager.nExtra bytes of local data follow the page data */
  /* 页面数据后面是 Pager.nExtra 字节的本地数据 */
};

/*
** Convert a pointer to a PgHdr into a pointer to its data
** and back again.
**
** 将指向PgHdr的指针转换为指向其数据的指针，反之亦然。
*/
#define PGHDR_TO_DATA(P) ((void *)(&(P)[1]))
#define DATA_TO_PGHDR(D) (&((PgHdr *)(D))[-1])
#define PGHDR_TO_EXTRA(P) ((void *)&((char *)(&(P)[1]))[SQLITE_PAGE_SIZE])

/*
** How big to make the hash table used for locating in-memory pages
** by page number.  Knuth says this should be a prime number.
**
** 用于按页码定位内存页面的哈希表的大小。Knuth说这应该是一个质数。
*/
#define N_PG_HASH 2003

/*
** A open page cache is an instance of the following structure.
**
** 打开的页面缓存是以下结构的实例。
*/
struct Pager {
  char *zFilename;         /* Name of the database file */
                           /* 数据库文件的名称 */
  char *zJournal;          /* Name of the journal file */
                           /* 日志文件的名称 */
  OsFile fd, jfd;          /* File descriptors for database and journal */
                           /* 数据库和日志的文件描述符 */
  OsFile cpfd;             /* File descriptor for the checkpoint journal */
                           /* 检查点日志的文件描述符 */
  int dbSize;              /* Number of pages in the file */
                           /* 文件中的页数 */
  int origDbSize;          /* dbSize before the current change */
                           /* 当前更改之前的 dbSize */
  int ckptSize, ckptJSize; /* Size of database and journal at ckpt_begin() */
                           /* ckpt_begin() 时的数据库和日志大小 */
  int nExtra;              /* Add this many bytes to each in-memory page */
                           /* 向每个内存页面添加这么多字节 */
  void (*xDestructor)(void *); /* Call this routine when freeing pages */
                               /* 释放页面时调用此例程 */
  int nPage;                   /* Total number of in-memory pages */
                               /* 内存页面的总数 */
  int nRef;                    /* Number of in-memory pages with PgHdr.nRef>0 */
                               /* PgHdr.nRef>0 的内存页面数 */
  int mxPage;                  /* Maximum number of pages to hold in cache */
                               /* 缓存中保留的最大页面数 */
  int nHit, nMiss, nOvfl;      /* Cache hits, missing, and LRU overflows */
                               /* 缓存命中、丢失和 LRU 溢出 */
  u8 journalOpen;              /* True if journal file descriptors is valid */
                               /* 如果日志文件描述符有效，则为 True */
  u8 ckptOpen;                 /* True if the checkpoint journal is open */
                               /* 如果检查点日志已打开，则为 True */
  u8 ckptInUse;                /* True we are in a checkpoint */
                               /* 如果我们在检查点中，则为 True */
  u8 noSync;                   /* Do not sync the journal if true */
                               /* 如果为 true，则不同步日志 */
  u8 state;                    /* SQLITE_UNLOCK, _READLOCK or _WRITELOCK */
                               /* SQLITE_UNLOCK, _READLOCK 或 _WRITELOCK */
  u8 errMask;                  /* One of several kinds of errors */
                               /* 几种错误之一 */
  u8 tempFile;                 /* zFilename is a temporary file */
                               /* zFilename 是一个临时文件 */
  u8 readOnly;                 /* True for a read-only database */
                               /* 对于只读数据库为 True */
  u8 needSync;                 /* True if an fsync() is needed on the journal */
                               /* 如果日志需要 fsync()，则为 True */
  u8 dirtyFile;            /* True if database file has changed in any way */
                           /* 如果数据库文件以任何方式更改，则为 True */
  u8 *aInJournal;          /* One bit for each page in the database file */
                           /* 数据库文件中每一页的一位 */
  u8 *aInCkpt;             /* One bit for each page in the database */
                           /* 数据库中每一页的一位 */
  PgHdr *pFirst, *pLast;   /* List of free pages */
                           /* 空闲页面列表 */
  PgHdr *pAll;             /* List of all pages */
                           /* 所有页面列表 */
  PgHdr *aHash[N_PG_HASH]; /* Hash table to map page number of PgHdr */
                           /* 映射 PgHdr 页码的哈希表 */
};

/*
** These are bits that can be set in Pager.errMask.
**
** 这些是可以在Pager.errMask中设置的位。
*/
#define PAGER_ERR_FULL 0x01    /* a write() failed */
                               /* write() 失败 */
#define PAGER_ERR_MEM 0x02     /* malloc() failed */
                               /* malloc() 失败 */
#define PAGER_ERR_LOCK 0x04    /* error in the locking protocol */
                               /* 锁定协议错误 */
#define PAGER_ERR_CORRUPT 0x08 /* database or journal corruption */
                               /* 数据库或日志损坏 */
#define PAGER_ERR_DISK 0x10    /* general disk I/O error - bad hard drive? */
                               /* 一般磁盘 I/O 错误 - 硬盘坏了？ */

/*
** The journal file contains page records in the following
** format.
**
** 日志文件包含以下格式的页面记录。
*/
typedef struct PageRecord PageRecord;
struct PageRecord {
  Pgno pgno;                    /* The page number */
                                /* 页码 */
  char aData[SQLITE_PAGE_SIZE]; /* Original data for page pgno */
                                /* 页面 pgno 的原始数据 */
};

/*
** Journal files begin with the following magic string.  The data
** was obtained from /dev/random.  It is used only as a sanity check.
**
** 日志文件以以下魔术字符串开头。数据是从/dev/random获取的。
** 它仅用作健全性检查。
*/
static const unsigned char aJournalMagic[] = {
    0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd4,
};

/*
** Hash a page number
**
** 哈希页码
*/
#define pager_hash(PN) ((PN) % N_PG_HASH)

/*
** Enable reference count tracking here:
**
** 在此处启用引用计数跟踪：
*/
#if SQLITE_TEST
int pager_refinfo_enable = 0;
static void pager_refinfo(PgHdr *p) {
  static int cnt = 0;
  if (!pager_refinfo_enable)
    return;
  printf("REFCNT: %4d addr=0x%08x nRef=%d\n", p->pgno, (int)PGHDR_TO_DATA(p),
         p->nRef);
  cnt++; /* Something to set a breakpoint on */
}
#define REFINFO(X) pager_refinfo(X)
#else
#define REFINFO(X)
#endif

/*
** Convert the bits in the pPager->errMask into an approprate
** return code.
**
** 将pPager->errMask中的位转换为适当的返回代码。
*/
static int pager_errcode(Pager *pPager) {
  int rc = SQLITE_OK;
  if (pPager->errMask & PAGER_ERR_LOCK)
    rc = SQLITE_PROTOCOL;
  if (pPager->errMask & PAGER_ERR_DISK)
    rc = SQLITE_IOERR;
  if (pPager->errMask & PAGER_ERR_FULL)
    rc = SQLITE_FULL;
  if (pPager->errMask & PAGER_ERR_MEM)
    rc = SQLITE_NOMEM;
  if (pPager->errMask & PAGER_ERR_CORRUPT)
    rc = SQLITE_CORRUPT;
  return rc;
}

/*
** Find a page in the hash table given its page number.  Return
** a pointer to the page or NULL if not found.
**
** 给定页码，在哈希表中查找页面。返回指向页面的指针，如果未找到则返回NULL。
*/
static PgHdr *pager_lookup(Pager *pPager, Pgno pgno) {
  PgHdr *p = pPager->aHash[pgno % N_PG_HASH];
  while (p && p->pgno != pgno) {
    p = p->pNextHash;
  }
  return p;
}

/*
** Unlock the database and clear the in-memory cache.  This routine
** sets the state of the pager back to what it was when it was first
** opened.  Any outstanding pages are invalidated and subsequent attempts
** to access those pages will likely result in a coredump.
**
** 解锁数据库并清除内存缓存。此例程将分页器的状态设置回首次打开时的状态。
** 任何未完成的页面都将失效，随后访问这些页面的尝试可能会导致核心转储。
*/
static void pager_reset(Pager *pPager) {
  PgHdr *pPg, *pNext;
  for (pPg = pPager->pAll; pPg; pPg = pNext) {
    pNext = pPg->pNextAll;
    sqliteFree(pPg);
  }
  pPager->pFirst = 0;
  pPager->pLast = 0;
  pPager->pAll = 0;
  memset(pPager->aHash, 0, sizeof(pPager->aHash));
  pPager->nPage = 0;
  if (pPager->state >= SQLITE_WRITELOCK) {
    sqlitepager_rollback(pPager);
  }
  sqliteOsUnlock(&pPager->fd);
  pPager->state = SQLITE_UNLOCK;
  pPager->dbSize = -1;
  pPager->nRef = 0;
  assert(pPager->journalOpen == 0);
}

/*
** When this routine is called, the pager has the journal file open and
** a write lock on the database.  This routine releases the database
** write lock and acquires a read lock in its place.  The journal file
** is deleted and closed.
**
** 当调用此例程时，分页器已打开日志文件并在数据库上拥有写锁。
** 此例程释放数据库写锁并获取读锁。日志文件被删除并关闭。
*/
static int pager_unwritelock(Pager *pPager) {
  int rc;
  PgHdr *pPg;
  if (pPager->state < SQLITE_WRITELOCK)
    return SQLITE_OK;
  sqlitepager_ckpt_commit(pPager);
  if (pPager->ckptOpen) {
    sqliteOsClose(&pPager->cpfd);
    pPager->ckptOpen = 0;
  }
  sqliteOsClose(&pPager->jfd);
  pPager->journalOpen = 0;
  sqliteOsDelete(pPager->zJournal);
  rc = sqliteOsReadLock(&pPager->fd);
  assert(rc == SQLITE_OK);
  sqliteFree(pPager->aInJournal);
  pPager->aInJournal = 0;
  for (pPg = pPager->pAll; pPg; pPg = pPg->pNextAll) {
    pPg->inJournal = 0;
    pPg->dirty = 0;
  }
  pPager->state = SQLITE_READLOCK;
  return rc;
}

/*
** Read a single page from the journal file opened on file descriptor
** jfd.  Playback this one page.
**
** 从在文件描述符jfd上打开的日志文件中读取单个页面。回放这一页。
*/
static int pager_playback_one_page(Pager *pPager, OsFile *jfd) {
  int rc;
  PgHdr *pPg; /* An existing page in the cache */
  PageRecord pgRec;

  rc = sqliteOsRead(jfd, &pgRec, sizeof(pgRec));
  if (rc != SQLITE_OK)
    return rc;

  /* Sanity checking on the page */
  if (pgRec.pgno > pPager->dbSize || pgRec.pgno == 0)
    return SQLITE_CORRUPT;

  /* Playback the page.  Update the in-memory copy of the page
  ** at the same time, if there is one.
  */
  pPg = pager_lookup(pPager, pgRec.pgno);
  if (pPg) {
    memcpy(PGHDR_TO_DATA(pPg), pgRec.aData, SQLITE_PAGE_SIZE);
    memset(PGHDR_TO_EXTRA(pPg), 0, pPager->nExtra);
  }
  rc = sqliteOsSeek(&pPager->fd, (pgRec.pgno - 1) * SQLITE_PAGE_SIZE);
  if (rc == SQLITE_OK) {
    rc = sqliteOsWrite(&pPager->fd, pgRec.aData, SQLITE_PAGE_SIZE);
  }
  return rc;
}

/*
** Playback the journal and thus restore the database file to
** the state it was in before we started making changes.
**
** The journal file format is as follows:  There is an initial
** file-type string for sanity checking.  Then there is a single
** Pgno number which is the number of pages in the database before
** changes were made.  The database is truncated to this size.
** Next come zero or more page records where each page record
** consists of a Pgno and SQLITE_PAGE_SIZE bytes of data.  See
** the PageRecord structure for details.
**
** If the file opened as the journal file is not a well-formed
** journal file (as determined by looking at the magic number
** at the beginning) then this routine returns SQLITE_PROTOCOL.
** If any other errors occur during playback, the database will
** likely be corrupted, so the PAGER_ERR_CORRUPT bit is set in
** pPager->errMask and SQLITE_CORRUPT is returned.  If it all
** works, then this routine returns SQLITE_OK.
**
** 回放日志，从而将数据库文件恢复到我们开始进行更改之前的状态。
**
** 日志文件格式如下：有一个用于健全性检查的初始文件类型字符串。
** 然后是一个Pgno数字，它是进行更改之前数据库中的页面数。
** 数据库被截断为此大小。接下来是零个或多个页面记录，
** 其中每个页面记录由一个Pgno和SQLITE_PAGE_SIZE字节的数据组成。
** 有关详细信息，请参阅PageRecord结构。
**
** 如果作为日志文件打开的文件不是格式良好的日志文件（通过查看开头的魔术数字确定），
** 则此例程返回SQLITE_PROTOCOL。如果在回放期间发生任何其他错误，
** 数据库可能会损坏，因此在pPager->errMask中设置PAGER_ERR_CORRUPT位，
** 并返回SQLITE_CORRUPT。如果一切正常，则此例程返回SQLITE_OK。
*/
static int pager_playback(Pager *pPager) {
  int nRec;      /* Number of Records */
  int i;         /* Loop counter */
  Pgno mxPg = 0; /* Size of the original file in pages */
  unsigned char aMagic[sizeof(aJournalMagic)];
  int rc;

  /* Figure out how many records are in the journal.  Abort early if
  ** the journal is empty.
  */
  assert(pPager->journalOpen);
  sqliteOsSeek(&pPager->jfd, 0);
  rc = sqliteOsFileSize(&pPager->jfd, &nRec);
  if (rc != SQLITE_OK) {
    goto end_playback;
  }
  nRec = (nRec - (sizeof(aMagic) + sizeof(Pgno))) / sizeof(PageRecord);
  if (nRec <= 0) {
    goto end_playback;
  }

  /* Read the beginning of the journal and truncate the
  ** database file back to its original size.
  */
  rc = sqliteOsRead(&pPager->jfd, aMagic, sizeof(aMagic));
  if (rc != SQLITE_OK || memcmp(aMagic, aJournalMagic, sizeof(aMagic)) != 0) {
    rc = SQLITE_PROTOCOL;
    goto end_playback;
  }
  rc = sqliteOsRead(&pPager->jfd, &mxPg, sizeof(mxPg));
  if (rc != SQLITE_OK) {
    goto end_playback;
  }
  rc = sqliteOsTruncate(&pPager->fd, mxPg * SQLITE_PAGE_SIZE);
  if (rc != SQLITE_OK) {
    goto end_playback;
  }
  pPager->dbSize = mxPg;

  /* Copy original pages out of the journal and back into the database file.
   */
  for (i = nRec - 1; i >= 0; i--) {
    rc = pager_playback_one_page(pPager, &pPager->jfd);
    if (rc != SQLITE_OK)
      break;
  }

end_playback:
  if (rc != SQLITE_OK) {
    pager_unwritelock(pPager);
    pPager->errMask |= PAGER_ERR_CORRUPT;
    rc = SQLITE_CORRUPT;
  } else {
    rc = pager_unwritelock(pPager);
  }
  return rc;
}

/*
** Playback the checkpoint journal.
**
** This is similar to playing back the transaction journal but with
** a few extra twists.
**
**    (1)  The number of pages in the database file at the start of
**         the checkpoint is stored in pPager->ckptSize, not in the
**         journal file itself.
**
**    (2)  In addition to playing back the checkpoint journal, also
**         playback all pages of the transaction journal beginning
**         at offset pPager->ckptJSize.
**
** 回放检查点日志。
**
** 这类似于回放事务日志，但有一些额外的曲折。
**
**    (1)  检查点开始时数据库文件中的页面数存储在pPager->ckptSize中，
**         而不是存储在日志文件本身中。
**
**    (2)  除了回放检查点日志外，还要回放从偏移量pPager->ckptJSize开始的
**         事务日志的所有页面。
*/
static int pager_ckpt_playback(Pager *pPager) {
  int nRec; /* Number of Records */
  int i;    /* Loop counter */
  int rc;

  /* Truncate the database back to its original size.
   */
  rc = sqliteOsTruncate(&pPager->fd, pPager->ckptSize * SQLITE_PAGE_SIZE);
  pPager->dbSize = pPager->ckptSize;

  /* Figure out how many records are in the checkpoint journal.
   */
  assert(pPager->ckptInUse && pPager->journalOpen);
  sqliteOsSeek(&pPager->cpfd, 0);
  rc = sqliteOsFileSize(&pPager->cpfd, &nRec);
  if (rc != SQLITE_OK) {
    goto end_ckpt_playback;
  }
  nRec /= sizeof(PageRecord);

  /* Copy original pages out of the checkpoint journal and back into the
  ** database file.
  */
  for (i = nRec - 1; i >= 0; i--) {
    rc = pager_playback_one_page(pPager, &pPager->cpfd);
    if (rc != SQLITE_OK)
      goto end_ckpt_playback;
  }

  /* Figure out how many pages need to be copied out of the transaction
  ** journal.
  */
  rc = sqliteOsSeek(&pPager->jfd, pPager->ckptJSize);
  if (rc != SQLITE_OK) {
    goto end_ckpt_playback;
  }
  rc = sqliteOsFileSize(&pPager->jfd, &nRec);
  if (rc != SQLITE_OK) {
    goto end_ckpt_playback;
  }
  nRec = (nRec - pPager->ckptJSize) / sizeof(PageRecord);
  for (i = nRec - 1; i >= 0; i--) {
    rc = pager_playback_one_page(pPager, &pPager->jfd);
    if (rc != SQLITE_OK)
      goto end_ckpt_playback;
  }

end_ckpt_playback:
  if (rc != SQLITE_OK) {
    pPager->errMask |= PAGER_ERR_CORRUPT;
    rc = SQLITE_CORRUPT;
  }
  return rc;
}

/*
** Change the maximum number of in-memory pages that are allowed.
**
** The maximum number is the absolute value of the mxPage parameter.
** If mxPage is negative, the noSync flag is also set.  noSync bypasses
** calls to sqliteOsSync().  The pager runs much faster with noSync on,
** but if the operating system crashes or there is an abrupt power
** failure, the database file might be left in an inconsistent and
** unrepairable state.
**
** 更改允许的最大内存页面数。
**
** 最大数量是mxPage参数的绝对值。如果mxPage为负数，则还会设置noSync标志。
** noSync绕过对sqliteOsSync()的调用。在打开noSync的情况下，分页器运行速度要快得多，
** 但如果操作系统崩溃或发生突然断电，数据库文件可能会处于不一致且无法修复的状态。
*/
void sqlitepager_set_cachesize(Pager *pPager, int mxPage) {
  if (mxPage >= 0) {
    pPager->noSync = pPager->tempFile;
  } else {
    pPager->noSync = 1;
    mxPage = -mxPage;
  }
  if (mxPage > 10) {
    pPager->mxPage = mxPage;
  }
}

/*
** Open a temporary file.  Write the name of the file into zName
** (zName must be at least SQLITE_TEMPNAME_SIZE bytes long.)  Write
** the file descriptor into *fd.  Return SQLITE_OK on success or some
** other error code if we fail.
**
** The OS will automatically delete the temporary file when it is
** closed.
**
** 打开一个临时文件。将文件名写入zName（zName必须至少为SQLITE_TEMPNAME_SIZE字节长。）
** 将文件描述符写入*fd。成功时返回SQLITE_OK，如果失败则返回其他错误代码。
**
** 操作系统将在关闭临时文件时自动将其删除。
*/
static int sqlitepager_opentemp(char *zFile, OsFile *fd) {
  int cnt = 8;
  int rc;
  do {
    cnt--;
    sqliteOsTempFileName(zFile);
    rc = sqliteOsOpenExclusive(zFile, fd, 1);
  } while (cnt > 0 && rc != SQLITE_OK);
  return rc;
}

/*
** Create a new page cache and put a pointer to the page cache in *ppPager.
** The file to be cached need not exist.  The file is not locked until
** the first call to sqlitepager_get() and is only held open until the
** last page is released using sqlitepager_unref().
**
** If zFilename is NULL then a randomly-named temporary file is created
** and used as the file to be cached.  The file will be deleted
** automatically when it is closed.
**
** 创建一个新的页面缓存，并将指向页面缓存的指针放入*ppPager中。
** 要缓存的文件不需要存在。直到第一次调用sqlitepager_get()时才锁定文件，
** 并且仅在释放最后一页（使用sqlitepager_unref()）之前保持打开状态。
**
** 如果zFilename为NULL，则创建一个随机命名的临时文件并将其用作要缓存的文件。
** 该文件将在关闭时自动删除。
*/
int sqlitepager_open(
    Pager **ppPager,       /* Return the Pager structure here */
    const char *zFilename, /* Name of the database file to open */
    int mxPage,            /* Max number of in-memory cache pages */
    int nExtra             /* Extra bytes append to each in-memory page */
) {
  Pager *pPager;
  int nameLen;
  OsFile fd;
  int rc;
  int tempFile;
  int readOnly = 0;
  char zTemp[SQLITE_TEMPNAME_SIZE];

  *ppPager = 0;
  if (sqlite_malloc_failed) {
    return SQLITE_NOMEM;
  }
  if (zFilename) {
    rc = sqliteOsOpenReadWrite(zFilename, &fd, &readOnly);
    tempFile = 0;
  } else {
    rc = sqlitepager_opentemp(zTemp, &fd);
    zFilename = zTemp;
    tempFile = 1;
  }
  if (rc != SQLITE_OK) {
    return SQLITE_CANTOPEN;
  }
  nameLen = strlen(zFilename);
  pPager = malloc(sizeof(*pPager) + nameLen * 2 + 30);
  memset(pPager, 0, sizeof(*pPager) + nameLen * 2 + 30);
  if (pPager == 0) {
    sqliteOsClose(&fd);
    return SQLITE_NOMEM;
  }
  pPager->zFilename = (char *)&pPager[1];
  pPager->zJournal = &pPager->zFilename[nameLen + 1];
  strcpy(pPager->zFilename, zFilename);
  strcpy(pPager->zJournal, zFilename);
  strcpy(&pPager->zJournal[nameLen], "-journal");
  pPager->fd = fd;
  pPager->journalOpen = 0;
  pPager->ckptOpen = 0;
  pPager->ckptInUse = 0;
  pPager->nRef = 0;
  pPager->dbSize = -1;
  pPager->ckptSize = 0;
  pPager->ckptJSize = 0;
  pPager->nPage = 0;
  pPager->mxPage = mxPage > 5 ? mxPage : 10;
  pPager->state = SQLITE_UNLOCK;
  pPager->errMask = 0;
  pPager->tempFile = tempFile;
  pPager->readOnly = readOnly;
  pPager->needSync = 0;
  pPager->noSync = pPager->tempFile;
  pPager->pFirst = 0;
  pPager->pLast = 0;
  pPager->nExtra = nExtra;
  memset(pPager->aHash, 0, sizeof(pPager->aHash));
  *ppPager = pPager;
  return SQLITE_OK;
}

/*
** Set the destructor for this pager.  If not NULL, the destructor is called
** when the reference count on each page reaches zero.  The destructor can
** be used to clean up information in the extra segment appended to each page.
**
** The destructor is not called as a result sqlitepager_close().
** Destructors are only called by sqlitepager_unref().
**
** 设置此分页器的析构函数。如果不为NULL，则当每个页面的引用计数达到零时调用析构函数。
** 析构函数可用于清理附加到每个页面的额外段中的信息。
**
** sqlitepager_close()不会导致调用析构函数。
** 析构函数仅由sqlitepager_unref()调用。
*/
void sqlitepager_set_destructor(Pager *pPager, void (*xDesc)(void *)) {
  pPager->xDestructor = xDesc;
}

/*
** Return the total number of pages in the disk file associated with
** pPager.
**
** 返回与pPager关联的磁盘文件中的总页数。
*/
int sqlitepager_pagecount(Pager *pPager) {
  int n;
  assert(pPager != 0);
  if (pPager->dbSize >= 0) {
    return pPager->dbSize;
  }
  if (sqliteOsFileSize(&pPager->fd, &n) != SQLITE_OK) {
    pPager->errMask |= PAGER_ERR_DISK;
    return 0;
  }
  n /= SQLITE_PAGE_SIZE;
  if (pPager->state != SQLITE_UNLOCK) {
    pPager->dbSize = n;
  }
  return n;
}

/*
** Shutdown the page cache.  Free all memory and close all files.
**
** If a transaction was in progress when this routine is called, that
** transaction is rolled back.  All outstanding pages are invalidated
** and their memory is freed.  Any attempt to use a page associated
** with this page cache after this function returns will likely
** result in a coredump.
**
** 关闭页面缓存。释放所有内存并关闭所有文件。
**
** 如果调用此例程时事务正在进行中，则回滚该事务。
** 所有未完成的页面都将失效，并且其内存将被释放。
** 在此函数返回后尝试使用与此页面缓存关联的页面可能会导致核心转储。
*/
int sqlitepager_close(Pager *pPager) {
  PgHdr *pPg, *pNext;
  switch (pPager->state) {
  case SQLITE_WRITELOCK: {
    sqlitepager_rollback(pPager);
    sqliteOsUnlock(&pPager->fd);
    assert(pPager->journalOpen == 0);
    break;
  }
  case SQLITE_READLOCK: {
    sqliteOsUnlock(&pPager->fd);
    break;
  }
  default: {
    /* Do nothing */
    break;
  }
  }
  for (pPg = pPager->pAll; pPg; pPg = pNext) {
    pNext = pPg->pNextAll;
    sqliteFree(pPg);
  }
  sqliteOsClose(&pPager->fd);
  assert(pPager->journalOpen == 0);
  /* Temp files are automatically deleted by the OS
  ** if( pPager->tempFile ){
  **   sqliteOsDelete(pPager->zFilename);
  ** }
  */
  sqliteFree(pPager);
  return SQLITE_OK;
}

/*
** Return the page number for the given page data.
**
** 返回给定页面数据的页码。
*/
Pgno sqlitepager_pagenumber(void *pData) {
  PgHdr *p = DATA_TO_PGHDR(pData);
  return p->pgno;
}

/*
** Increment the reference count for a page.  If the page is
** currently on the freelist (the reference count is zero) then
** remove it from the freelist.
**
** 增加页面的引用计数。如果页面当前在空闲列表中（引用计数为零），
** 则将其从空闲列表中删除。
*/
static void page_ref(PgHdr *pPg) {
  if (pPg->nRef == 0) {
    /* The page is currently on the freelist.  Remove it. */
    if (pPg->pPrevFree) {
      pPg->pPrevFree->pNextFree = pPg->pNextFree;
    } else {
      pPg->pPager->pFirst = pPg->pNextFree;
    }
    if (pPg->pNextFree) {
      pPg->pNextFree->pPrevFree = pPg->pPrevFree;
    } else {
      pPg->pPager->pLast = pPg->pPrevFree;
    }
    pPg->pPager->nRef++;
  }
  pPg->nRef++;
  REFINFO(pPg);
}

/*
** Increment the reference count for a page.  The input pointer is
** a reference to the page data.
**
** 增加页面的引用计数。输入指针是对页面数据的引用。
*/
int sqlitepager_ref(void *pData) {
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  page_ref(pPg);
  return SQLITE_OK;
}

/*
** Sync the journal and then write all free dirty pages to the database
** file.
**
** Writing all free dirty pages to the database after the sync is a
** non-obvious optimization.  fsync() is an expensive operation so we
** want to minimize the number ot times it is called. After an fsync() call,
** we are free to write dirty pages back to the database.  It is best
** to go ahead and write as many dirty pages as possible to minimize
** the risk of having to do another fsync() later on.  Writing dirty
** free pages in this way was observed to make database operations go
** up to 10 times faster.
**
** If we are writing to temporary database, there is no need to preserve
** the integrity of the journal file, so we can save time and skip the
** fsync().
**
** 同步日志，然后将所有空闲脏页写入数据库文件。
**
** 在同步后将所有空闲脏页写入数据库是一个不明显的优化。
** fsync()是一个昂贵的操作，所以我们要尽量减少调用它的次数。
** 在调用fsync()之后，我们可以自由地将脏页写回数据库。
** 最好继续写入尽可能多的脏页，以尽量减少以后必须再次进行fsync()的风险。
** 据观察，以这种方式写入脏空闲页面可使数据库操作速度提高多达10倍。
**
** 如果我们正在写入临时数据库，则无需保留日志文件的完整性，
** 因此我们可以节省时间并跳过fsync()。
*/
static int syncAllPages(Pager *pPager) {
  PgHdr *pPg;
  int rc = SQLITE_OK;
  if (pPager->needSync) {
    if (!pPager->tempFile) {
      rc = sqliteOsSync(&pPager->jfd);
      if (rc != 0)
        return rc;
    }
    pPager->needSync = 0;
  }
  for (pPg = pPager->pFirst; pPg; pPg = pPg->pNextFree) {
    if (pPg->dirty) {
      sqliteOsSeek(&pPager->fd, (pPg->pgno - 1) * SQLITE_PAGE_SIZE);
      rc = sqliteOsWrite(&pPager->fd, PGHDR_TO_DATA(pPg), SQLITE_PAGE_SIZE);
      if (rc != SQLITE_OK)
        break;
      pPg->dirty = 0;
    }
  }
  return rc;
}

/*
** Acquire a page.
**
** A read lock on the disk file is obtained when the first page is acquired.
** This read lock is dropped when the last page is released.
**
** A _get works for any page number greater than 0.  If the database
** file is smaller than the requested page, then no actual disk
** read occurs and the memory image of the page is initialized to
** all zeros.  The extra data appended to a page is always initialized
** to zeros the first time a page is loaded into memory.
**
** The acquisition might fail for several reasons.  In all cases,
** an appropriate error code is returned and *ppPage is set to NULL.
**
** See also sqlitepager_lookup().  Both this routine and _lookup() attempt
** to find a page in the in-memory cache first.  If the page is not already
** in memory, this routine goes to disk to read it in whereas _lookup()
** just returns 0.  This routine acquires a read-lock the first time it
** has to go to disk, and could also playback an old journal if necessary.
** Since _lookup() never goes to disk, it never has to deal with locks
** or journal files.
**
** 获取页面。
**
** 获取第一页时，将获得磁盘文件的读锁。释放最后一页时，将删除此读锁。
**
** _get适用于任何大于0的页码。如果数据库文件小于请求的页面，
** 则不会发生实际的磁盘读取，并且页面的内存映像将初始化为全零。
** 附加到页面的额外数据在页面首次加载到内存时始终初始化为零。
**
** 获取可能会因多种原因而失败。在所有情况下，都会返回适当的错误代码，
** 并将*ppPage设置为NULL。
**
** 另请参阅sqlitepager_lookup()。此例程和_lookup()都尝试首先在内存缓存中查找页面。
** 如果页面尚未在内存中，则此例程会转到磁盘读取它，而_lookup()仅返回0。
** 此例程在第一次必须转到磁盘时获取读锁，并且如果需要，还可以回放旧日志。
** 由于_lookup()从不转到磁盘，因此它从不需要处理锁或日志文件。
*/
int sqlitepager_get(Pager *pPager, Pgno pgno, void **ppPage) {
  PgHdr *pPg;

  /* Make sure we have not hit any critical errors.
   */
  if (pPager == 0 || pgno == 0) {
    return SQLITE_ERROR;
  }
  if (pPager->errMask & ~(PAGER_ERR_FULL)) {
    return pager_errcode(pPager);
  }

  /* If this is the first page accessed, then get a read lock
  ** on the database file.
  **
  ** 如果这是访问的第一个页面，则获取数据库文件的读锁。
  */
  if (pPager->nRef == 0) {
    if (sqliteOsReadLock(&pPager->fd) != SQLITE_OK) {
      *ppPage = 0;
      return SQLITE_BUSY;
    }
    pPager->state = SQLITE_READLOCK;

    /* If a journal file exists, try to play it back.
    **
    ** 如果存在日志文件，请尝试回放它。
    */
    if (sqliteOsFileExists(pPager->zJournal)) {
      int rc, dummy;

      /* Get a write lock on the database
      **
      ** 获取数据库的写锁
      */
      rc = sqliteOsWriteLock(&pPager->fd);
      if (rc != SQLITE_OK) {
        rc = sqliteOsUnlock(&pPager->fd);
        assert(rc == SQLITE_OK);
        *ppPage = 0;
        return SQLITE_BUSY;
      }
      pPager->state = SQLITE_WRITELOCK;

      /* Open the journal for exclusive access.  Return SQLITE_BUSY if
      ** we cannot get exclusive access to the journal file.
      **
      ** Even though we will only be reading from the journal, not writing,
      ** we have to open the journal for writing in order to obtain an
      ** exclusive access lock.
      **
      ** 打开日志以进行独占访问。如果我们无法获得对日志文件的独占访问权限，
      ** 则返回SQLITE_BUSY。
      **
      ** 即使我们只从日志中读取而不写入，我们也必须打开日志进行写入，
      ** 以便获得独占访问锁。
      */
      rc = sqliteOsOpenReadWrite(pPager->zJournal, &pPager->jfd, &dummy);
      if (rc != SQLITE_OK) {
        rc = sqliteOsUnlock(&pPager->fd);
        assert(rc == SQLITE_OK);
        *ppPage = 0;
        return SQLITE_BUSY;
      }
      pPager->journalOpen = 1;

      /* Playback and delete the journal.  Drop the database write
      ** lock and reacquire the read lock.
      **
      ** 回放并删除日志。删除数据库写锁并重新获取读锁。
      */
      rc = pager_playback(pPager);
      if (rc != SQLITE_OK) {
        return rc;
      }
    }
    pPg = 0;
  } else {
    /* Search for page in cache
    **
    ** 在缓存中搜索页面
    */
    pPg = pager_lookup(pPager, pgno);
  }
  if (pPg == 0) {
    /* The requested page is not in the page cache.
    **
    ** 请求的页面不在页面缓存中。
    */
    int h;
    pPager->nMiss++;
    if (pPager->nPage < pPager->mxPage || pPager->pFirst == 0) {
      /* Create a new page
      **
      ** 创建一个新页面
      */
      pPg = malloc(sizeof(*pPg) + SQLITE_PAGE_SIZE + pPager->nExtra);
      memset(pPg, 0, sizeof(*pPg) + SQLITE_PAGE_SIZE + pPager->nExtra);
      if (pPg == 0) {
        *ppPage = 0;
        pager_unwritelock(pPager);
        pPager->errMask |= PAGER_ERR_MEM;
        return SQLITE_NOMEM;
      }
      pPg->pPager = pPager;
      pPg->pNextAll = pPager->pAll;
      if (pPager->pAll) {
        pPager->pAll->pPrevAll = pPg;
      }
      pPg->pPrevAll = 0;
      pPager->pAll = pPg;
      pPager->nPage++;
    } else {
      /* Recycle an older page.  First locate the page to be recycled.
      ** Try to find one that is not dirty and is near the head of
      ** of the free list
      **
      ** 回收旧页面。首先找到要回收的页面。
      ** 尝试找到一个不脏且靠近空闲列表头部的页面
      */
      pPg = pPager->pFirst;
      while (pPg && pPg->dirty) {
        pPg = pPg->pNextFree;
      }

      /* If we could not find a page that has not been used recently
      ** and which is not dirty, then sync the journal and write all
      ** dirty free pages into the database file, thus making them
      ** clean pages and available for recycling.
      **
      ** We have to sync the journal before writing a page to the main
      ** database.  But syncing is a very slow operation.  So after a
      ** sync, it is best to write everything we can back to the main
      ** database to minimize the risk of having to sync again in the
      ** near future.  That is way we write all dirty pages after a
      ** sync.
      **
      ** 如果我们找不到最近未使用且不脏的页面，则同步日志并将所有
      ** 脏空闲页面写入数据库文件，从而使它们成为干净页面并可用于回收。
      **
      ** 在将页面写入主数据库之前，我们必须同步日志。但同步是一个非常慢的操作。
      ** 因此，在同步之后，最好将我们能写的所有内容都写回主数据库，
      ** 以尽量减少在不久的将来再次同步的风险。这就是我们在同步后写入所有脏页的原因。
      */
      if (pPg == 0) {
        int rc = syncAllPages(pPager);
        if (rc != 0) {
          sqlitepager_rollback(pPager);
          *ppPage = 0;
          return SQLITE_IOERR;
        }
        pPg = pPager->pFirst;
      }
      assert(pPg->nRef == 0);
      assert(pPg->dirty == 0);

      /* Unlink the old page from the free list and the hash table
      **
      ** 从空闲列表和哈希表中取消链接旧页面
      */
      if (pPg->pPrevFree) {
        pPg->pPrevFree->pNextFree = pPg->pNextFree;
      } else {
        assert(pPager->pFirst == pPg);
        pPager->pFirst = pPg->pNextFree;
      }
      if (pPg->pNextFree) {
        pPg->pNextFree->pPrevFree = pPg->pPrevFree;
      } else {
        assert(pPager->pLast == pPg);
        pPager->pLast = pPg->pPrevFree;
      }
      pPg->pNextFree = pPg->pPrevFree = 0;
      if (pPg->pNextHash) {
        pPg->pNextHash->pPrevHash = pPg->pPrevHash;
      }
      if (pPg->pPrevHash) {
        pPg->pPrevHash->pNextHash = pPg->pNextHash;
      } else {
        h = pager_hash(pPg->pgno);
        assert(pPager->aHash[h] == pPg);
        pPager->aHash[h] = pPg->pNextHash;
      }
      pPg->pNextHash = pPg->pPrevHash = 0;
      pPager->nOvfl++;
    }
    pPg->pgno = pgno;
    if (pPager->aInJournal && (int)pgno <= pPager->origDbSize) {
      pPg->inJournal = (pPager->aInJournal[pgno / 8] & (1 << (pgno & 7))) != 0;
    } else {
      pPg->inJournal = 0;
    }
    if (pPager->aInCkpt && (int)pgno <= pPager->ckptSize) {
      pPg->inCkpt = (pPager->aInCkpt[pgno / 8] & (1 << (pgno & 7))) != 0;
    } else {
      pPg->inCkpt = 0;
    }
    pPg->dirty = 0;
    pPg->nRef = 1;
    REFINFO(pPg);
    pPager->nRef++;
    h = pager_hash(pgno);
    pPg->pNextHash = pPager->aHash[h];
    pPager->aHash[h] = pPg;
    if (pPg->pNextHash) {
      assert(pPg->pNextHash->pPrevHash == 0);
      pPg->pNextHash->pPrevHash = pPg;
    }
    if (pPager->dbSize < 0)
      sqlitepager_pagecount(pPager);
    if (pPager->dbSize < (int)pgno) {
      memset(PGHDR_TO_DATA(pPg), 0, SQLITE_PAGE_SIZE);
    } else {
      int rc;
      sqliteOsSeek(&pPager->fd, (pgno - 1) * SQLITE_PAGE_SIZE);
      rc = sqliteOsRead(&pPager->fd, PGHDR_TO_DATA(pPg), SQLITE_PAGE_SIZE);
      if (rc != SQLITE_OK) {
        return rc;
      }
    }
    if (pPager->nExtra > 0) {
      memset(PGHDR_TO_EXTRA(pPg), 0, pPager->nExtra);
    }
  } else {
    /* The requested page is in the page cache.
    **
    ** 请求的页面在页面缓存中。
    */
    pPager->nHit++;
    page_ref(pPg);
  }
  *ppPage = PGHDR_TO_DATA(pPg);
  return SQLITE_OK;
}

/*
** Acquire a page if it is already in the in-memory cache.  Do
** not read the page from disk.  Return a pointer to the page,
** or 0 if the page is not in cache.
**
** See also sqlitepager_get().  The difference between this routine
** and sqlitepager_get() is that _get() will go to the disk and read
** in the page if the page is not already in cache.  This routine
** returns NULL if the page is not in cache or if a disk I/O error
** has ever happened.
**
** 如果页面已在内存缓存中，则获取该页面。不要从磁盘读取页面。
** 返回指向页面的指针，如果页面不在缓存中，则返回0。
**
** 另请参阅sqlitepager_get()。此例程与sqlitepager_get()的区别在于，
** 如果页面尚未在缓存中，_get()将转到磁盘并读取页面。
** 如果页面不在缓存中或发生过磁盘I/O错误，此例程将返回NULL。
*/
void *sqlitepager_lookup(Pager *pPager, Pgno pgno) {
  PgHdr *pPg;

  /* Make sure we have not hit any critical errors.
  **
  ** 确保我们没有遇到任何严重错误。
  */
  if (pPager == 0 || pgno == 0) {
    return 0;
  }
  if (pPager->errMask & ~(PAGER_ERR_FULL)) {
    return 0;
  }
  if (pPager->nRef == 0) {
    return 0;
  }
  pPg = pager_lookup(pPager, pgno);
  if (pPg == 0)
    return 0;
  page_ref(pPg);
  return PGHDR_TO_DATA(pPg);
}

/*
** Release a page.
**
** If the number of references to the page drop to zero, then the
** page is added to the LRU list.  When all references to all pages
** are released, a rollback occurs and the lock on the database is
** removed.
**
** 释放页面。
**
** 如果页面的引用数降至零，则将该页面添加到LRU列表。
** 当所有页面的所有引用都被释放时，将发生回滚并删除数据库上的锁。
*/
int sqlitepager_unref(void *pData) {
  PgHdr *pPg;

  /* Decrement the reference count for this page
  **
  ** 减少此页面的引用计数
  */
  pPg = DATA_TO_PGHDR(pData);
  assert(pPg->nRef > 0);
  pPg->nRef--;
  REFINFO(pPg);

  /* When the number of references to a page reach 0, call the
  ** destructor and add the page to the freelist.
  **
  ** 当页面的引用数达到0时，调用析构函数并将页面添加到空闲列表。
  */
  if (pPg->nRef == 0) {
    Pager *pPager;
    pPager = pPg->pPager;
    pPg->pNextFree = 0;
    pPg->pPrevFree = pPager->pLast;
    pPager->pLast = pPg;
    if (pPg->pPrevFree) {
      pPg->pPrevFree->pNextFree = pPg;
    } else {
      pPager->pFirst = pPg;
    }
    if (pPager->xDestructor) {
      pPager->xDestructor(pData);
    }

    /* When all pages reach the freelist, drop the read lock from
    ** the database file.
    **
    ** 当所有页面都到达空闲列表时，从数据库文件中删除读锁。
    */
    pPager->nRef--;
    assert(pPager->nRef >= 0);
    if (pPager->nRef == 0) {
      pager_reset(pPager);
    }
  }
  return SQLITE_OK;
}

/*
** Acquire a write-lock on the database.  The lock is removed when
** the any of the following happen:
**
**   *  sqlitepager_commit() is called.
**   *  sqlitepager_rollback() is called.
**   *  sqlitepager_close() is called.
**   *  sqlitepager_unref() is called to on every outstanding page.
**
** The parameter to this routine is a pointer to any open page of the
** database file.  Nothing changes about the page - it is used merely
** to acquire a pointer to the Pager structure and as proof that there
** is already a read-lock on the database.
**
** If the database is already write-locked, this routine is a no-op.
**
** 获取数据库的写锁。当发生以下任何情况时，将删除该锁：
**
**   *  调用sqlitepager_commit()。
**   *  调用sqlitepager_rollback()。
**   *  调用sqlitepager_close()。
**   *  对每个未完成的页面调用sqlitepager_unref()。
**
** 此例程的参数是指向数据库文件任何打开页面的指针。
** 页面没有任何变化 - 它仅用于获取指向Pager结构的指针，
** 并作为数据库上已有读锁的证明。
**
** 如果数据库已被写锁定，则此例程为无操作。
*/
int sqlitepager_begin(void *pData) {
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  Pager *pPager = pPg->pPager;
  int rc = SQLITE_OK;
  assert(pPg->nRef > 0);
  assert(pPager->state != SQLITE_UNLOCK);
  if (pPager->state == SQLITE_READLOCK) {
    assert(pPager->aInJournal == 0);
    rc = sqliteOsWriteLock(&pPager->fd);
    if (rc != SQLITE_OK) {
      return rc;
    }
    pPager->aInJournal = malloc(pPager->dbSize / 8 + 1);
    memset(pPager->aInJournal, 0, pPager->dbSize / 8 + 1);

    if (pPager->aInJournal == 0) {
      sqliteOsReadLock(&pPager->fd);
      return SQLITE_NOMEM;
    }
    rc = sqliteOsOpenExclusive(pPager->zJournal, &pPager->jfd, 0);
    if (rc != SQLITE_OK) {
      sqliteFree(pPager->aInJournal);
      pPager->aInJournal = 0;
      sqliteOsReadLock(&pPager->fd);
      return SQLITE_CANTOPEN;
    }
    pPager->journalOpen = 1;
    pPager->needSync = 0;
    pPager->dirtyFile = 0;
    pPager->state = SQLITE_WRITELOCK;
    sqlitepager_pagecount(pPager);
    pPager->origDbSize = pPager->dbSize;
    rc = sqliteOsWrite(&pPager->jfd, aJournalMagic, sizeof(aJournalMagic));
    if (rc == SQLITE_OK) {
      rc = sqliteOsWrite(&pPager->jfd, &pPager->dbSize, sizeof(Pgno));
    }
    if (rc != SQLITE_OK) {
      rc = pager_unwritelock(pPager);
      if (rc == SQLITE_OK)
        rc = SQLITE_FULL;
    }
  }
  return rc;
}

/*
** Mark a data page as writeable.  The page is written into the journal
** if it is not there already.  This routine must be called before making
** changes to a page.
**
** The first time this routine is called, the pager creates a new
** journal and acquires a write lock on the database.  If the write
** lock could not be acquired, this routine returns SQLITE_BUSY.  The
** calling routine must check for that return value and be careful not to
** change any page data until this routine returns SQLITE_OK.
**
** If the journal file could not be written because the disk is full,
** then this routine returns SQLITE_FULL and does an immediate rollback.
** All subsequent write attempts also return SQLITE_FULL until there
** is a call to sqlitepager_commit() or sqlitepager_rollback() to
** reset.
**
** 将数据页标记为可写。如果页面尚未在日志中，则将其写入日志。
** 在更改页面之前必须调用此例程。
**
** 第一次调用此例程时，分页器会创建一个新日志并获取数据库的写锁。
** 如果无法获取写锁，此例程将返回SQLITE_BUSY。
** 调用例程必须检查该返回值，并在该例程返回SQLITE_OK之前小心不要更改任何页面数据。
**
** 如果由于磁盘已满而无法写入日志文件，则此例程返回SQLITE_FULL并立即回滚。
** 所有后续写入尝试也返回SQLITE_FULL，直到调用sqlitepager_commit()或
** sqlitepager_rollback()进行重置。
*/
int sqlitepager_write(void *pData) {
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  Pager *pPager = pPg->pPager;
  int rc = SQLITE_OK;

  /* Check for errors
  **
  ** 检查错误
  */
  if (pPager->errMask) {
    return pager_errcode(pPager);
  }
  if (pPager->readOnly) {
    return SQLITE_PERM;
  }

  /* Mark the page as dirty.  If the page has already been written
  ** to the journal then we can return right away.
  **
  ** 将页面标记为脏。如果页面已写入日志，我们可以立即返回。
  */
  pPg->dirty = 1;
  if (pPg->inJournal && (pPg->inCkpt || pPager->ckptInUse == 0)) {
    pPager->dirtyFile = 1;
    return SQLITE_OK;
  }

  /* If we get this far, it means that the page needs to be
  ** written to the transaction journal or the ckeckpoint journal
  ** or both.
  **
  ** First check to see that the transaction journal exists and
  ** create it if it does not.
  **
  ** 如果我们到了这一步，这意味着页面需要写入事务日志或检查点日志，或两者兼而有之。
  **
  ** 首先检查事务日志是否存在，如果不存在则创建它。
  */
  assert(pPager->state != SQLITE_UNLOCK);
  rc = sqlitepager_begin(pData);
  pPager->dirtyFile = 1;
  if (rc != SQLITE_OK)
    return rc;
  assert(pPager->state == SQLITE_WRITELOCK);
  assert(pPager->journalOpen);

  /* The transaction journal now exists and we have a write lock on the
  ** main database file.  Write the current page to the transaction
  ** journal if it is not there already.
  **
  ** 事务日志现在存在，并且我们在主数据库文件上拥有写锁。
  ** 如果当前页面尚未在事务日志中，则将其写入事务日志。
  */
  if (!pPg->inJournal && (int)pPg->pgno <= pPager->origDbSize) {
    rc = sqliteOsWrite(&pPager->jfd, &pPg->pgno, sizeof(Pgno));
    if (rc == SQLITE_OK) {
      rc = sqliteOsWrite(&pPager->jfd, pData, SQLITE_PAGE_SIZE);
    }
    if (rc != SQLITE_OK) {
      sqlitepager_rollback(pPager);
      pPager->errMask |= PAGER_ERR_FULL;
      return rc;
    }
    assert(pPager->aInJournal != 0);
    pPager->aInJournal[pPg->pgno / 8] |= 1 << (pPg->pgno & 7);
    pPager->needSync = !pPager->noSync;
    pPg->inJournal = 1;
    if (pPager->ckptInUse) {
      pPager->aInCkpt[pPg->pgno / 8] |= 1 << (pPg->pgno & 7);
      pPg->inCkpt = 1;
    }
  }

  /* If the checkpoint journal is open and the page is not in it,
  ** then write the current page to the checkpoint journal.
  **
  ** 如果检查点日志已打开且页面不在其中，则将当前页面写入检查点日志。
  */
  if (pPager->ckptInUse && !pPg->inCkpt && (int)pPg->pgno <= pPager->ckptSize) {
    assert(pPg->inJournal || (int)pPg->pgno > pPager->origDbSize);
    rc = sqliteOsWrite(&pPager->cpfd, &pPg->pgno, sizeof(Pgno));
    if (rc == SQLITE_OK) {
      rc = sqliteOsWrite(&pPager->cpfd, pData, SQLITE_PAGE_SIZE);
    }
    if (rc != SQLITE_OK) {
      sqlitepager_rollback(pPager);
      pPager->errMask |= PAGER_ERR_FULL;
      return rc;
    }
    assert(pPager->aInCkpt != 0);
    pPager->aInCkpt[pPg->pgno / 8] |= 1 << (pPg->pgno & 7);
    pPg->inCkpt = 1;
  }

  /* Update the database size and return.
  **
  ** 更新数据库大小并返回。
  */
  if (pPager->dbSize < (int)pPg->pgno) {
    pPager->dbSize = pPg->pgno;
  }
  return rc;
}

/*
** Return TRUE if the page given in the argument was previously passed
** to sqlitepager_write().  In other words, return TRUE if it is ok
** to change the content of the page.
**
** 如果参数中给出的页面之前已传递给sqlitepager_write()，则返回TRUE。
** 换句话说，如果可以更改页面的内容，则返回TRUE。
*/
int sqlitepager_iswriteable(void *pData) {
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  return pPg->dirty;
}

/*
** A call to this routine tells the pager that it is not necessary to
** write the information on page "pgno" back to the disk, even though
** that page might be marked as dirty.
**
** The overlying software layer calls this routine when all of the data
** on the given page is unused.  The pager marks the page as clean so
** that it does not get written to disk.
**
** Tests show that this optimization, together with the
** sqlitepager_dont_rollback() below, more than double the speed
** of large INSERT operations and quadruple the speed of large DELETEs.
**
** 调用此例程告诉分页器，即使页面“pgno”可能被标记为脏，
** 也不必将其信息写回磁盘。
**
** 当给定页面上的所有数据都未使用时，上层软件层会调用此例程。
** 分页器将页面标记为干净，以便不会将其写入磁盘。
**
** 测试表明，此优化与下面的sqlitepager_dont_rollback()一起，
** 使大型INSERT操作的速度提高了一倍以上，并使大型DELETE操作的速度提高了四倍。
*/
void sqlitepager_dont_write(Pager *pPager, Pgno pgno) {
  PgHdr *pPg;
  pPg = pager_lookup(pPager, pgno);
  if (pPg && pPg->dirty) {
    pPg->dirty = 0;
  }
}

/*
** A call to this routine tells the pager that if a rollback occurs,
** it is not necessary to restore the data on the given page.  This
** means that the pager does not have to record the given page in the
** rollback journal.
**
** 调用此例程告诉分页器，如果发生回滚，则不必恢复给定页面上的数据。
** 这意味着分页器不必在回滚日志中记录给定页面。
*/
void sqlitepager_dont_rollback(void *pData) {
  PgHdr *pPg = DATA_TO_PGHDR(pData);
  Pager *pPager = pPg->pPager;

  if (pPager->state != SQLITE_WRITELOCK || pPager->journalOpen == 0)
    return;
  if (!pPg->inJournal && (int)pPg->pgno <= pPager->origDbSize) {
    assert(pPager->aInJournal != 0);
    pPager->aInJournal[pPg->pgno / 8] |= 1 << (pPg->pgno & 7);
    pPg->inJournal = 1;
    if (pPager->ckptInUse) {
      pPager->aInCkpt[pPg->pgno / 8] |= 1 << (pPg->pgno & 7);
      pPg->inCkpt = 1;
    }
  }
  if (pPager->ckptInUse && !pPg->inCkpt && (int)pPg->pgno <= pPager->ckptSize) {
    assert(pPg->inJournal || (int)pPg->pgno > pPager->origDbSize);
    assert(pPager->aInCkpt != 0);
    pPager->aInCkpt[pPg->pgno / 8] |= 1 << (pPg->pgno & 7);
    pPg->inCkpt = 1;
  }
}

/*
** Commit all changes to the database and release the write lock.
**
** If the commit fails for any reason, a rollback attempt is made
** and an error code is returned.  If the commit worked, SQLITE_OK
** is returned.
**
** 提交对数据库的所有更改并释放写锁。
**
** 如果提交因任何原因失败，则尝试回滚并返回错误代码。
** 如果提交成功，则返回SQLITE_OK。
*/
int sqlitepager_commit(Pager *pPager) {
  int rc;
  PgHdr *pPg;

  if (pPager->errMask == PAGER_ERR_FULL) {
    rc = sqlitepager_rollback(pPager);
    if (rc == SQLITE_OK)
      rc = SQLITE_FULL;
    return rc;
  }
  if (pPager->errMask != 0) {
    rc = pager_errcode(pPager);
    return rc;
  }
  if (pPager->state != SQLITE_WRITELOCK) {
    return SQLITE_ERROR;
  }
  assert(pPager->journalOpen);
  if (pPager->dirtyFile == 0) {
    /* Exit early (without doing the time-consuming sqliteOsSync() calls)
    ** if there have been no changes to the database file. */
    /* 如果数据库文件没有更改，则提前退出（不执行耗时的sqliteOsSync()调用）。 */
    rc = pager_unwritelock(pPager);
    pPager->dbSize = -1;
    return rc;
  }
  if (pPager->needSync && sqliteOsSync(&pPager->jfd) != SQLITE_OK) {
    goto commit_abort;
  }
  for (pPg = pPager->pAll; pPg; pPg = pPg->pNextAll) {
    if (pPg->dirty == 0)
      continue;
    rc = sqliteOsSeek(&pPager->fd, (pPg->pgno - 1) * SQLITE_PAGE_SIZE);
    if (rc != SQLITE_OK)
      goto commit_abort;
    rc = sqliteOsWrite(&pPager->fd, PGHDR_TO_DATA(pPg), SQLITE_PAGE_SIZE);
    if (rc != SQLITE_OK)
      goto commit_abort;
  }
  if (!pPager->noSync && sqliteOsSync(&pPager->fd) != SQLITE_OK) {
    goto commit_abort;
  }
  rc = pager_unwritelock(pPager);
  pPager->dbSize = -1;
  return rc;

  /* Jump here if anything goes wrong during the commit process.
  **
  ** 如果提交过程中出现任何问题，请跳转至此处。
  */
commit_abort:
  rc = sqlitepager_rollback(pPager);
  if (rc == SQLITE_OK) {
    rc = SQLITE_FULL;
  }
  return rc;
}

/*
** Rollback all changes.  The database falls back to read-only mode.
** All in-memory cache pages revert to their original data contents.
** The journal is deleted.
**
** This routine cannot fail unless some other process is not following
** the correct locking protocol (SQLITE_PROTOCOL) or unless some other
** process is writing trash into the journal file (SQLITE_CORRUPT) or
** unless a prior malloc() failed (SQLITE_NOMEM).  Appropriate error
** codes are returned for all these occasions.  Otherwise,
** SQLITE_OK is returned.
**
** 回滚所有更改。数据库回退到只读模式。
** 所有内存缓存页面都恢复为其原始数据内容。日志被删除。
**
** 除非其他进程未遵循正确的锁定协议（SQLITE_PROTOCOL），
** 或者其他进程正在将垃圾写入日志文件（SQLITE_CORRUPT），
** 或者先前的malloc()失败（SQLITE_NOMEM），否则此例程不会失败。
** 在所有这些情况下都会返回适当的错误代码。否则，返回SQLITE_OK。
*/
int sqlitepager_rollback(Pager *pPager) {
  int rc;
  if (pPager->errMask != 0 && pPager->errMask != PAGER_ERR_FULL) {
    if (pPager->state >= SQLITE_WRITELOCK) {
      pager_playback(pPager);
    }
    return pager_errcode(pPager);
  }
  if (pPager->state != SQLITE_WRITELOCK) {
    return SQLITE_OK;
  }
  rc = pager_playback(pPager);
  if (rc != SQLITE_OK) {
    rc = SQLITE_CORRUPT;
    pPager->errMask |= PAGER_ERR_CORRUPT;
  }
  pPager->dbSize = -1;
  return rc;
}

/*
** Return TRUE if the database file is opened read-only.  Return FALSE
** if the database is (in theory) writable.
**
** 如果数据库文件以只读方式打开，则返回TRUE。
** 如果数据库（理论上）可写，则返回FALSE。
*/
int sqlitepager_isreadonly(Pager *pPager) { return pPager->readOnly; }

/*
** This routine is used for testing and analysis only.
**
** 此例程仅用于测试和分析。
*/
int *sqlitepager_stats(Pager *pPager) {
  static int a[9];
  a[0] = pPager->nRef;
  a[1] = pPager->nPage;
  a[2] = pPager->mxPage;
  a[3] = pPager->dbSize;
  a[4] = pPager->state;
  a[5] = pPager->errMask;
  a[6] = pPager->nHit;
  a[7] = pPager->nMiss;
  a[8] = pPager->nOvfl;
  return a;
}

/*
** Set the checkpoint.
**
** This routine should be called with the transaction journal already
** open.  A new checkpoint journal is created that can be used to rollback
** changes of a single SQL command within a larger transaction.
**
** 设置检查点。
**
** 应在事务日志已打开的情况下调用此例程。
** 创建一个新的检查点日志，可用于回滚较大事务中单个SQL命令的更改。
*/
int sqlitepager_ckpt_begin(Pager *pPager) {
  int rc;
  char zTemp[SQLITE_TEMPNAME_SIZE];
  assert(pPager->journalOpen);
  assert(!pPager->ckptInUse);
  pPager->aInCkpt = malloc(pPager->dbSize / 8 + 1);
  memset(pPager->aInCkpt, 0, pPager->dbSize / 8 + 1);
  if (pPager->aInCkpt == 0) {
    sqliteOsReadLock(&pPager->fd);
    return SQLITE_NOMEM;
  }
  rc = sqliteOsFileSize(&pPager->jfd, &pPager->ckptJSize);
  if (rc)
    goto ckpt_begin_failed;
  pPager->ckptSize = pPager->dbSize;
  if (!pPager->ckptOpen) {
    rc = sqlitepager_opentemp(zTemp, &pPager->cpfd);
    if (rc)
      goto ckpt_begin_failed;
    pPager->ckptOpen = 1;
  }
  pPager->ckptInUse = 1;
  return SQLITE_OK;

ckpt_begin_failed:
  if (pPager->aInCkpt) {
    sqliteFree(pPager->aInCkpt);
    pPager->aInCkpt = 0;
  }
  return rc;
}

/*
** Commit a checkpoint.
**
** 提交检查点。
*/
int sqlitepager_ckpt_commit(Pager *pPager) {
  if (pPager->ckptInUse) {
    PgHdr *pPg;
    sqliteOsTruncate(&pPager->cpfd, 0);
    pPager->ckptInUse = 0;
    sqliteFree(pPager->aInCkpt);
    pPager->aInCkpt = 0;
    for (pPg = pPager->pAll; pPg; pPg = pPg->pNextAll) {
      pPg->inCkpt = 0;
    }
  }
  return SQLITE_OK;
}

/*
** Rollback a checkpoint.
**
** 回滚检查点。
*/
int sqlitepager_ckpt_rollback(Pager *pPager) {
  int rc;
  if (pPager->ckptInUse) {
    rc = pager_ckpt_playback(pPager);
    sqlitepager_ckpt_commit(pPager);
  } else {
    rc = SQLITE_OK;
  }
  return rc;
}

#if SQLITE_TEST
/*
** Print a listing of all referenced pages and their ref count.
**
** 打印所有引用页面及其引用计数的列表。
*/
void sqlitepager_refdump(Pager *pPager) {
  PgHdr *pPg;
  for (pPg = pPager->pAll; pPg; pPg = pPg->pNextAll) {
    if (pPg->nRef <= 0)
      continue;
    printf("PAGE %3d addr=0x%08x nRef=%d\n", pPg->pgno, (int)PGHDR_TO_DATA(pPg),
           pPg->nRef);
  }
}
#endif
