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
** $Id: btree.c,v 1.62 2002/06/06 23:16:05 drh Exp $
**
** This file implements a external (disk-based) database using BTrees.
** For a detailed discussion of BTrees, refer to
**
**     Donald E. Knuth, THE ART OF COMPUTER PROGRAMMING, Volume 3:
**     "Sorting And Searching", pages 473-480. Addison-Wesley
**     Publishing Company, Reading, Massachusetts.
**
** The basic idea is that each page of the file contains N database
** entries and N+1 pointers to subpages.
**
**   ----------------------------------------------------------------
**   |  Ptr(0) | Key(0) | Ptr(1) | Key(1) | ... | Key(N) | Ptr(N+1) |
**   ----------------------------------------------------------------
**
** All of the keys on the page that Ptr(0) points to have values less
** than Key(0).  All of the keys on page Ptr(1) and its subpages have
** values greater than Key(0) and less than Key(1).  All of the keys
** on Ptr(N+1) and its subpages have values greater than Key(N).  And
** so forth.
**
** Finding a particular key requires reading O(log(M)) pages from the
** disk where M is the number of entries in the tree.
**
** In this implementation, a single file can hold one or more separate
** BTrees.  Each BTree is identified by the index of its root page.  The
** key and data for any entry are combined to form the "payload".  Up to
** MX_LOCAL_PAYLOAD bytes of payload can be carried directly on the
** database page.  If the payload is larger than MX_LOCAL_PAYLOAD bytes
** then surplus bytes are stored on overflow pages.  The payload for an
** entry and the preceding pointer are combined to form a "Cell".  Each
** page has a small header which contains the Ptr(N+1) pointer.
**
** The first page of the file contains a magic string used to verify that
** the file really is a valid BTree database, a pointer to a list of unused
** pages in the file, and some meta information.  The root of the first
** BTree begins on page 2 of the file.  (Pages are numbered beginning with
** 1, not 0.)  Thus a minimum database contains 2 pages.
**
** 2001年9月15日
**
** 作者放弃此源代码的版权。作为法律声明的替代，这里有一个祝福：
**
**    愿你行善，不作恶。
**    愿你原谅自己，也原谅他人。
**    愿你慷慨分享，索取不超所予。
**
*************************************************************************
** $Id: btree.c,v 1.62 2002/06/06 23:16:05 drh Exp $
**
** 本文件实现了一个使用B树的外部（基于磁盘）数据库。
** 关于B树的详细讨论，请参考：
**
**     Donald E. Knuth, THE ART OF COMPUTER PROGRAMMING, Volume 3:
**     "Sorting And Searching", pages 473-480. Addison-Wesley
**     Publishing Company, Reading, Massachusetts.
**
** 基本思想是文件的每一页包含N个数据库条目和N+1个指向子页的指针。
**
**   ----------------------------------------------------------------
**   |  Ptr(0) | Key(0) | Ptr(1) | Key(1) | ... | Key(N) | Ptr(N+1) |
**   ----------------------------------------------------------------
**
** Ptr(0)指向的页面上的所有键值都小于Key(0)。Ptr(1)及其子页上的所有键值都
** 大于Key(0)且小于Key(1)。Ptr(N+1)及其子页上的所有键值都大于Key(N)。
** 以此类推。
**
** 查找特定键需要从磁盘读取O(log(M))页，其中M是树中的条目数。
**
** 在此实现中，单个文件可以容纳一个或多个独立的B树。每个B树由其根页的索引标识。
** 任何条目的键和数据组合形成“有效载荷”。最多MX_LOCAL_PAYLOAD字节的有效载荷
** 可以直接携带在数据库页面上。如果有效载荷大于MX_LOCAL_PAYLOAD字节，
** 则多余的字节存储在溢出页上。条目的有效载荷和前面的指针组合形成一个“单元”。
** 每个页面都有一个包含Ptr(N+1)指针的小标题。
**
** 文件的第一页包含一个魔术字符串，用于验证文件是否确实是有效的B树数据库，
** 一个指向文件中未使用页面列表的指针，以及一些元信息。第一个B树的根从文件的
** 第2页开始。（页面从1开始编号，而不是0。）因此，最小的数据库包含2页。
*/
#include "btree.h"
#include "hash.h"
#include "pager.h"
#include "sqlite.h"
#include "sqliteInt.h"
#include "util.h"
#include <assert.h>

/*
** Forward declarations of structures used only in this file.
**
** 仅在本文件中使用的结构的前向声明。
*/
typedef struct PageOne PageOne;
typedef struct MemPage MemPage;
typedef struct PageHdr PageHdr;
typedef struct Cell Cell;
typedef struct CellHdr CellHdr;
typedef struct FreeBlk FreeBlk;
typedef struct OverflowPage OverflowPage;
typedef struct FreelistInfo FreelistInfo;

/*
** All structures on a database page are aligned to 4-byte boundries.
** This routine rounds up a number of bytes to the next multiple of 4.
**
** This might need to change for computer architectures that require
** and 8-byte alignment boundry for structures.
**
** 数据库页面上的所有结构都对齐到4字节边界。
** 此例程将字节数向上舍入为4的下一个倍数。
**
** 对于需要8字节对齐边界的计算机体系结构，这可能需要更改。
*/
#define ROUNDUP(X) ((X + 3) & ~3)

/*
** This is a magic string that appears at the beginning of every
** SQLite database in order to identify the file as a real database.
**
** 这是一个出现在每个SQLite数据库开头的魔术字符串，
** 用于将文件标识为真实的数据库。
*/
static const char zMagicHeader[] =
    "** This file contains an SQLite 2.1 database **";
#define MAGIC_SIZE (sizeof(zMagicHeader))

/*
** This is a magic integer also used to test the integrity of the database
** file.  This integer is used in addition to the string above so that
** if the file is written on a little-endian architecture and read
** on a big-endian architectures (or vice versa) we can detect the
** problem.
**
** The number used was obtained at random and has no special
** significance other than the fact that it represents a different
** integer on little-endian and big-endian machines.
**
** 这是一个魔术整数，也用于测试数据库文件的完整性。
** 除了上面的字符串外，还使用此整数，以便如果文件是在小端架构上写入
** 并在大端架构上读取（反之亦然），我们可以检测到该问题。
**
** 所使用的数字是随机获得的，除了它在小端和大端机器上表示不同的整数
** 这一事实外，没有特殊意义。
*/
#define MAGIC 0xdae37528

/*
** The first page of the database file contains a magic header string
** to identify the file as an SQLite database file.  It also contains
** a pointer to the first free page of the file.  Page 2 contains the
** root of the principle BTree.  The file might contain other BTrees
** rooted on pages above 2.
**
** The first page also contains SQLITE_N_BTREE_META integers that
** can be used by higher-level routines.
**
** Remember that pages are numbered beginning with 1.  (See pager.c
** for additional information.)  Page 0 does not exist and a page
** number of 0 is used to mean "no such page".
**
** 数据库文件的第一页包含一个魔术头字符串，用于将文件标识为SQLite数据库文件。
** 它还包含指向文件第一个空闲页面的指针。第2页包含主B树的根。
** 文件可能包含以2以上页面为根的其他B树。
**
** 第一页还包含SQLITE_N_BTREE_META个整数，可供更高级别的例程使用。
**
** 请记住，页面从1开始编号。（有关更多信息，请参阅pager.c。）
** 第0页不存在，页码0用于表示“无此页面”。
*/
struct PageOne {
  char zMagic[MAGIC_SIZE]; /* String that identifies the file as a database */
                           /* 标识文件为数据库的字符串 */
  int iMagic;              /* Integer to verify correct byte order */
                           /* 用于验证正确字节顺序的整数 */
  Pgno freeList;           /* First free page in a list of all free pages */
                           /* 所有空闲页面列表中的第一个空闲页面 */
  int nFree;               /* Number of pages on the free list */
                           /* 空闲列表上的页面数 */
  int aMeta[SQLITE_N_BTREE_META - 1]; /* User defined integers */
                                      /* 用户定义的整数 */
};

/*
** Each database page has a header that is an instance of this
** structure.
**
** PageHdr.firstFree is 0 if there is no free space on this page.
** Otherwise, PageHdr.firstFree is the index in MemPage.u.aDisk[] of a
** FreeBlk structure that describes the first block of free space.
** All free space is defined by a linked list of FreeBlk structures.
**
** Data is stored in a linked list of Cell structures.  PageHdr.firstCell
** is the index into MemPage.u.aDisk[] of the first cell on the page.  The
** Cells are kept in sorted order.
**
** A Cell contains all information about a database entry and a pointer
** to a child page that contains other entries less than itself.  In
** other words, the i-th Cell contains both Ptr(i) and Key(i).  The
** right-most pointer of the page is contained in PageHdr.rightChild.
**
** 每个数据库页面都有一个作为此结构实例的标题。
**
** 如果此页面上没有可用空间，则PageHdr.firstFree为0。
** 否则，PageHdr.firstFree是描述第一个空闲空间块的FreeBlk结构在
** MemPage.u.aDisk[]中的索引。所有空闲空间由FreeBlk结构的链表定义。
**
** 数据存储在Cell结构的链表中。PageHdr.firstCell是页面上第一个单元在
** MemPage.u.aDisk[]中的索引。单元保持排序顺序。
**
** 单元包含有关数据库条目的所有信息以及指向包含小于其自身的其他条目的
** 子页面的指针。换句话说，第i个单元包含Ptr(i)和Key(i)。
** 页面的最右侧指针包含在PageHdr.rightChild中。
*/
struct PageHdr {
  Pgno rightChild; /* Child page that comes after all cells on this page */
                   /* 此页面上所有单元之后的子页面 */
  u16 firstCell;   /* Index in MemPage.u.aDisk[] of the first cell */
                   /* 第一个单元在 MemPage.u.aDisk[] 中的索引 */
  u16 firstFree;   /* Index in MemPage.u.aDisk[] of the first free block */
                   /* 第一个空闲块在 MemPage.u.aDisk[] 中的索引 */
};

/*
** Entries on a page of the database are called "Cells".  Each Cell
** has a header and data.  This structure defines the header.  The
** key and data (collectively the "payload") follow this header on
** the database page.
**
** A definition of the complete Cell structure is given below.  The
** header for the cell must be defined first in order to do some
** of the sizing #defines that follow.
**
** 数据库页面上的条目称为“单元”。每个单元都有一个标题和数据。
** 此结构定义了标题。键和数据（统称为“有效载荷”）在数据库页面上
** 紧随此标题之后。
**
** 下面给出了完整Cell结构的定义。必须首先定义单元的标题，
** 以便进行后面的一些大小调整#defines。
*/
struct CellHdr {
  Pgno leftChild; /* Child page that comes before this cell */
                  /* 此单元之前的子页面 */
  u16 nKey;       /* Number of bytes in the key */
                  /* 键中的字节数 */
  u16 iNext;      /* Index in MemPage.u.aDisk[] of next cell in sorted order */
                  /* 排序顺序中下一个单元在 MemPage.u.aDisk[] 中的索引 */
  u8 nKeyHi;      /* Upper 8 bits of key size for keys larger than 64K bytes */
                  /* 大于 64K 字节的键的键大小的高 8 位 */
  u8 nDataHi;     /* Upper 8 bits of data size when the size is more than 64K */
                  /* 当大小超过 64K 时数据大小的高 8 位 */
  u16 nData;      /* Number of bytes of data */
                  /* 数据字节数 */
};

/*
** The key and data size are split into a lower 16-bit segment and an
** upper 8-bit segment in order to pack them together into a smaller
** space.  The following macros reassembly a key or data size back
** into an integer.
**
** 键和数据大小分为低16位段和高8位段，以便将它们打包到更小的空间中。
** 以下宏将键或数据大小重新组合回整数。
*/
#define NKEY(h) (h.nKey + h.nKeyHi * 65536)
#define NDATA(h) (h.nData + h.nDataHi * 65536)

/*
** The minimum size of a complete Cell.  The Cell must contain a header
** and at least 4 bytes of payload.
**
** 完整单元的最小大小。单元必须包含一个标题和至少4个字节的有效载荷。
*/
#define MIN_CELL_SIZE (sizeof(CellHdr) + 4)

/*
** The maximum number of database entries that can be held in a single
** page of the database.
**
** 单个数据库页面中可以容纳的最大数据库条目数。
*/
#define MX_CELL ((SQLITE_PAGE_SIZE - sizeof(PageHdr)) / MIN_CELL_SIZE)

/*
** The amount of usable space on a single page of the BTree.  This is the
** page size minus the overhead of the page header.
**
** B树单个页面上的可用空间量。这是页面大小减去页面标题的开销。
*/
#define USABLE_SPACE (SQLITE_PAGE_SIZE - sizeof(PageHdr))

/*
** The maximum amount of payload (in bytes) that can be stored locally for
** a database entry.  If the entry contains more data than this, the
** extra goes onto overflow pages.
**
** This number is chosen so that at least 4 cells will fit on every page.
**
** 数据库条目可以在本地存储的最大有效载荷量（以字节为单位）。
** 如果条目包含的数据超过此数量，则多余的数据将放入溢出页。
**
** 选择此数字是为了确保每页至少可以容纳4个单元。
*/
#define MX_LOCAL_PAYLOAD                                                       \
  ((USABLE_SPACE / 4 - (sizeof(CellHdr) + sizeof(Pgno))) & ~3)

/*
** Data on a database page is stored as a linked list of Cell structures.
** Both the key and the data are stored in aPayload[].  The key always comes
** first.  The aPayload[] field grows as necessary to hold the key and data,
** up to a maximum of MX_LOCAL_PAYLOAD bytes.  If the size of the key and
** data combined exceeds MX_LOCAL_PAYLOAD bytes, then Cell.ovfl is the
** page number of the first overflow page.
**
** Though this structure is fixed in size, the Cell on the database
** page varies in size.  Every cell has a CellHdr and at least 4 bytes
** of payload space.  Additional payload bytes (up to the maximum of
** MX_LOCAL_PAYLOAD) and the Cell.ovfl value are allocated only as
** needed.
**
** 数据库页面上的数据存储为Cell结构的链表。键和数据都存储在aPayload[]中。
** 键总是排在第一位。aPayload[]字段根据需要增长以容纳键和数据，
** 最多为MX_LOCAL_PAYLOAD字节。如果键和数据的总大小超过MX_LOCAL_PAYLOAD字节，
** 则Cell.ovfl是第一个溢出页的页码。
**
** 虽然此结构的大小是固定的，但数据库页面上的Cell大小是变化的。
** 每个单元都有一个CellHdr和至少4个字节的有效载荷空间。
** 额外的有效载荷字节（最多到MX_LOCAL_PAYLOAD）和Cell.ovfl值仅在需要时分配。
*/
struct Cell {
  CellHdr h;                       /* The cell header */
                                   /* 单元标题 */
  char aPayload[MX_LOCAL_PAYLOAD]; /* Key and data */
                                   /* 键和数据 */
  Pgno ovfl;                       /* The first overflow page */
                                   /* 第一个溢出页 */
};

/*
** Free space on a page is remembered using a linked list of the FreeBlk
** structures.  Space on a database page is allocated in increments of
** at least 4 bytes and is always aligned to a 4-byte boundry.  The
** linked list of FreeBlks is always kept in order by address.
**
** 页面上的空闲空间使用FreeBlk结构的链表来记录。
** 数据库页面上的空间以至少4字节为增量进行分配，并且始终对齐到4字节边界。
** FreeBlk的链表始终按地址顺序保存。
*/
struct FreeBlk {
  u16 iSize; /* Number of bytes in this block of free space */
             /* 此空闲空间块中的字节数 */
  u16 iNext; /* Index in MemPage.u.aDisk[] of the next free block */
             /* 下一个空闲块在 MemPage.u.aDisk[] 中的索引 */
};

/*
** The number of bytes of payload that will fit on a single overflow page.
**
** 单个溢出页上可容纳的有效载荷字节数。
*/
#define OVERFLOW_SIZE (SQLITE_PAGE_SIZE - sizeof(Pgno))

/*
** When the key and data for a single entry in the BTree will not fit in
** the MX_LOCAL_PAYLOAD bytes of space available on the database page,
** then all extra bytes are written to a linked list of overflow pages.
** Each overflow page is an instance of the following structure.
**
** Unused pages in the database are also represented by instances of
** the OverflowPage structure.  The PageOne.freeList field is the
** page number of the first page in a linked list of unused database
** pages.
**
** 当B树中单个条目的键和数据无法容纳在数据库页面上可用的MX_LOCAL_PAYLOAD字节空间中时，
** 所有多余的字节都将写入溢出页的链表中。每个溢出页都是以下结构的一个实例。
**
** 数据库中未使用的页面也由OverflowPage结构的实例表示。
** PageOne.freeList字段是未使用数据库页面链表中第一页的页码。
*/
struct OverflowPage {
  Pgno iNext;
  char aPayload[OVERFLOW_SIZE];
};

/*
** The PageOne.freeList field points to a linked list of overflow pages
** hold information about free pages.  The aPayload section of each
** overflow page contains an instance of the following structure.  The
** aFree[] array holds the page number of nFree unused pages in the disk
** file.
**
** PageOne.freeList字段指向保存有关空闲页面信息的溢出页链表。
** 每个溢出页的aPayload部分包含以下结构的一个实例。
** aFree[]数组保存磁盘文件中nFree个未使用页面的页码。
*/
struct FreelistInfo {
  int nFree;
  Pgno aFree[(OVERFLOW_SIZE - sizeof(int)) / sizeof(Pgno)];
};

/*
** For every page in the database file, an instance of the following structure
** is stored in memory.  The u.aDisk[] array contains the raw bits read from
** the disk.  The rest is auxiliary information held in memory only. The
** auxiliary info is only valid for regular database pages - it is not
** used for overflow pages and pages on the freelist.
**
** Of particular interest in the auxiliary info is the apCell[] entry.  Each
** apCell[] entry is a pointer to a Cell structure in u.aDisk[].  The cells are
** put in this array so that they can be accessed in constant time, rather
** than in linear time which would be needed if we had to walk the linked
** list on every access.
**
** Note that apCell[] contains enough space to hold up to two more Cells
** than can possibly fit on one page.  In the steady state, every apCell[]
** points to memory inside u.aDisk[].  But in the middle of an insert
** operation, some apCell[] entries may temporarily point to data space
** outside of u.aDisk[].  This is a transient situation that is quickly
** resolved.  But while it is happening, it is possible for a database
** page to hold as many as two more cells than it might otherwise hold.
** The extra two entries in apCell[] are an allowance for this situation.
**
** The pParent field points back to the parent page.  This allows us to
** walk up the BTree from any leaf to the root.  Care must be taken to
** unref() the parent page pointer when this page is no longer referenced.
** The pageDestructor() routine handles that chore.
**
** 对于数据库文件中的每一页，内存中都存储了以下结构的一个实例。
** u.aDisk[]数组包含从磁盘读取的原始位。其余部分是仅保存在内存中的辅助信息。
** 辅助信息仅对常规数据库页面有效 - 它不用于溢出页和空闲列表上的页面。
**
** 辅助信息中特别令人感兴趣的是apCell[]条目。每个apCell[]条目都是指向
** u.aDisk[]中Cell结构的指针。将单元放入此数组中，以便可以在恒定时间内访问它们，
** 而不是像我们每次访问都必须遍历链表那样需要线性时间。
**
** 请注意，apCell[]包含足够的空间来容纳比一页上可能容纳的单元多两个的单元。
** 在稳定状态下，每个apCell[]都指向u.aDisk[]内部的内存。但在插入操作中间，
** 一些apCell[]条目可能会暂时指向u.aDisk[]之外的数据空间。
** 这是一个很快就会解决的瞬态情况。但在发生这种情况时，数据库页面可能会容纳
** 比其他情况下多两个的单元。apCell[]中的额外两个条目是对这种情况的预留。
**
** pParent字段指回父页面。这允许我们从任何叶子向上遍历B树到根。
** 当不再引用此页面时，必须注意unref()父页面指针。
** pageDestructor()例程处理该杂务。
*/
struct MemPage {
  union {
    char aDisk[SQLITE_PAGE_SIZE]; /* Page data stored on disk */
                                  /* 存储在磁盘上的页面数据 */
    PageHdr hdr;                  /* Overlay page header */
                                  /* 覆盖页面标题 */
  } u;
  int isInit;                /* True if auxiliary data is initialized */
                             /* 如果辅助数据已初始化，则为 True */
  MemPage *pParent;          /* The parent of this page.  NULL for root */
                             /* 此页面的父级。根为 NULL */
  int nFree;                 /* Number of free bytes in u.aDisk[] */
                             /* u.aDisk[] 中的空闲字节数 */
  int nCell;                 /* Number of entries on this page */
                             /* 此页面上的条目数 */
  int isOverfull;            /* Some apCell[] points outside u.aDisk[] */
                             /* 一些 apCell[] 指向 u.aDisk[] 之外 */
  Cell *apCell[MX_CELL + 2]; /* All data entires in sorted order */
                             /* 按排序顺序的所有数据条目 */
};

/*
** The in-memory image of a disk page has the auxiliary information appended
** to the end.  EXTRA_SIZE is the number of bytes of space needed to hold
** that extra information.
**
** 磁盘页面的内存映像在末尾附加了辅助信息。
** EXTRA_SIZE是保存该额外信息所需的空间字节数。
*/
#define EXTRA_SIZE (sizeof(MemPage) - SQLITE_PAGE_SIZE)

/*
** Everything we need to know about an open database
**
** 关于打开的数据库我们需要知道的一切
*/
struct Btree {
  Pager *pPager;     /* The page cache */
                     /* 页面缓存 */
  BtCursor *pCursor; /* A list of all open cursors */
                     /* 所有打开游标的列表 */
  PageOne *page1;    /* First page of the database */
                     /* 数据库的第一页 */
  u8 inTrans;        /* True if a transaction is in progress */
                     /* 如果事务正在进行中，则为 True */
  u8 inCkpt;         /* True if there is a checkpoint on the transaction */
                     /* 如果事务上有检查点，则为 True */
  u8 readOnly;       /* True if the underlying file is readonly */
                     /* 如果底层文件是只读的，则为 True */
  Hash locks;        /* Key: root page number.  Data: lock count */
                     /* 键：根页码。数据：锁计数 */
};
typedef Btree Bt;

/*
** A cursor is a pointer to a particular entry in the BTree.
** The entry is identified by its MemPage and the index in
** MemPage.apCell[] of the entry.
**
** 游标是指向B树中特定条目的指针。
** 该条目由其MemPage和该条目在MemPage.apCell[]中的索引标识。
*/
struct BtCursor {
  Btree *pBt;              /* The Btree to which this cursor belongs */
                           /* 此游标所属的 Btree */
  BtCursor *pNext, *pPrev; /* Forms a linked list of all cursors */
                           /* 形成所有游标的链表 */
  Pgno pgnoRoot;           /* The root page of this tree */
                           /* 此树的根页 */
  MemPage *pPage;          /* Page that contains the entry */
                           /* 包含条目的页面 */
  int idx;                 /* Index of the entry in pPage->apCell[] */
                           /* 条目在 pPage->apCell[] 中的索引 */
  u8 wrFlag;               /* True if writable */
                           /* 如果可写，则为 True */
  u8 bSkipNext;            /* sqliteBtreeNext() is no-op if true */
                           /* 如果为 true，则 sqliteBtreeNext() 为空操作 */
  u8 iMatch;               /* compare result from last sqliteBtreeMoveto() */
                           /* 上次 sqliteBtreeMoveto() 的比较结果 */
};

/*
** Compute the total number of bytes that a Cell needs on the main
** database page.  The number returned includes the Cell header,
** local payload storage, and the pointer to overflow pages (if
** applicable).  Additional space allocated on overflow pages
** is NOT included in the value returned from this routine.
**
** 计算Cell在主数据库页面上所需的总字节数。
** 返回的数字包括Cell标题、本地有效载荷存储和指向溢出页的指针（如果适用）。
** 在溢出页上分配的额外空间不包含在此例程返回的值中。
*/
static int cellSize(Cell *pCell) {
  int n = NKEY(pCell->h) + NDATA(pCell->h);
  if (n > MX_LOCAL_PAYLOAD) {
    n = MX_LOCAL_PAYLOAD + sizeof(Pgno);
  } else {
    n = ROUNDUP(n);
  }
  n += sizeof(CellHdr);
  return n;
}

/*
** Defragment the page given.  All Cells are moved to the
** beginning of the page and all free space is collected
** into one big FreeBlk at the end of the page.
**
** 对给定页面进行碎片整理。所有单元都移动到页面开头，
** 所有空闲空间都收集到页面末尾的一个大FreeBlk中。
*/
static void defragmentPage(MemPage *pPage) {
  int pc, i, n;
  FreeBlk *pFBlk;
  char newPage[SQLITE_PAGE_SIZE];

  assert(sqlitepager_iswriteable(pPage));
  pc = sizeof(PageHdr);
  pPage->u.hdr.firstCell = pc;
  memcpy(newPage, pPage->u.aDisk, pc);
  for (i = 0; i < pPage->nCell; i++) {
    Cell *pCell = pPage->apCell[i];

    /* This routine should never be called on an overfull page.  The
    ** following asserts verify that constraint. */
    assert(Addr(pCell) > Addr(pPage));
    assert(Addr(pCell) < Addr(pPage) + SQLITE_PAGE_SIZE);

    n = cellSize(pCell);
    pCell->h.iNext = pc + n;
    memcpy(&newPage[pc], pCell, n);
    pPage->apCell[i] = (Cell *)&pPage->u.aDisk[pc];
    pc += n;
  }
  assert(pPage->nFree == SQLITE_PAGE_SIZE - pc);
  memcpy(pPage->u.aDisk, newPage, pc);
  if (pPage->nCell > 0) {
    pPage->apCell[pPage->nCell - 1]->h.iNext = 0;
  }
  pFBlk = (FreeBlk *)&pPage->u.aDisk[pc];
  pFBlk->iSize = SQLITE_PAGE_SIZE - pc;
  pFBlk->iNext = 0;
  pPage->u.hdr.firstFree = pc;
  memset(&pFBlk[1], 0, SQLITE_PAGE_SIZE - pc - sizeof(FreeBlk));
}

/*
** Allocate nByte bytes of space on a page.  nByte must be a
** multiple of 4.
**
** Return the index into pPage->u.aDisk[] of the first byte of
** the new allocation. Or return 0 if there is not enough free
** space on the page to satisfy the allocation request.
**
** If the page contains nBytes of free space but does not contain
** nBytes of contiguous free space, then this routine automatically
** calls defragementPage() to consolidate all free space before
** allocating the new chunk.
**
** 在页面上分配nByte字节的空间。nByte必须是4的倍数。
**
** 返回新分配的第一个字节在pPage->u.aDisk[]中的索引。
** 或者如果页面上没有足够的可用空间来满足分配请求，则返回0。
**
** 如果页面包含nBytes的可用空间但不包含nBytes的连续可用空间，
** 则此例程在分配新块之前自动调用defragementPage()来合并所有可用空间。
*/
static int allocateSpace(MemPage *pPage, int nByte) {
  FreeBlk *p;
  u16 *pIdx;
  int start;
  int cnt = 0;

  assert(sqlitepager_iswriteable(pPage));
  assert(nByte == ROUNDUP(nByte));
  if (pPage->nFree < nByte || pPage->isOverfull)
    return 0;
  pIdx = &pPage->u.hdr.firstFree;
  p = (FreeBlk *)&pPage->u.aDisk[*pIdx];
  while (p->iSize < nByte) {
    assert(cnt++ < SQLITE_PAGE_SIZE / 4);
    if (p->iNext == 0) {
      defragmentPage(pPage);
      pIdx = &pPage->u.hdr.firstFree;
    } else {
      pIdx = &p->iNext;
    }
    p = (FreeBlk *)&pPage->u.aDisk[*pIdx];
  }
  if (p->iSize == nByte) {
    start = *pIdx;
    *pIdx = p->iNext;
  } else {
    FreeBlk *pNew;
    start = *pIdx;
    pNew = (FreeBlk *)&pPage->u.aDisk[start + nByte];
    pNew->iNext = p->iNext;
    pNew->iSize = p->iSize - nByte;
    *pIdx = start + nByte;
  }
  pPage->nFree -= nByte;
  return start;
}

/*
** Return a section of the MemPage.u.aDisk[] to the freelist.
** The first byte of the new free block is pPage->u.aDisk[start]
** and the size of the block is "size" bytes.  Size must be
** a multiple of 4.
**
** Most of the effort here is involved in coalesing adjacent
** free blocks into a single big free block.
**
** 将MemPage.u.aDisk[]的一部分返回到空闲列表。
** 新空闲块的第一个字节是pPage->u.aDisk[start]，块的大小是“size”字节。
** Size必须是4的倍数。
**
** 这里的大部分工作涉及将相邻的空闲块合并为一个大的空闲块。
*/
static void freeSpace(MemPage *pPage, int start, int size) {
  int end = start + size;
  u16 *pIdx, idx;
  FreeBlk *pFBlk;
  FreeBlk *pNew;
  FreeBlk *pNext;

  assert(sqlitepager_iswriteable(pPage));
  assert(size == ROUNDUP(size));
  assert(start == ROUNDUP(start));
  pIdx = &pPage->u.hdr.firstFree;
  idx = *pIdx;
  while (idx != 0 && idx < start) {
    pFBlk = (FreeBlk *)&pPage->u.aDisk[idx];
    if (idx + pFBlk->iSize == start) {
      pFBlk->iSize += size;
      if (idx + pFBlk->iSize == pFBlk->iNext) {
        pNext = (FreeBlk *)&pPage->u.aDisk[pFBlk->iNext];
        pFBlk->iSize += pNext->iSize;
        pFBlk->iNext = pNext->iNext;
      }
      pPage->nFree += size;
      return;
    }
    pIdx = &pFBlk->iNext;
    idx = *pIdx;
  }
  pNew = (FreeBlk *)&pPage->u.aDisk[start];
  if (idx != end) {
    pNew->iSize = size;
    pNew->iNext = idx;
  } else {
    pNext = (FreeBlk *)&pPage->u.aDisk[idx];
    pNew->iSize = size + pNext->iSize;
    pNew->iNext = pNext->iNext;
  }
  *pIdx = start;
  pPage->nFree += size;
}

/*
** Initialize the auxiliary information for a disk block.
**
** The pParent parameter must be a pointer to the MemPage which
** is the parent of the page being initialized.  The root of the
** BTree (usually page 2) has no parent and so for that page,
** pParent==NULL.
**
** Return SQLITE_OK on success.  If we see that the page does
** not contained a well-formed database page, then return
** SQLITE_CORRUPT.  Note that a return of SQLITE_OK does not
** guarantee that the page is well-formed.  It only shows that
** we failed to detect any corruption.
**
** 初始化磁盘块的辅助信息。
**
** pParent参数必须是指向正在初始化的页面的父MemPage的指针。
** B树的根（通常是第2页）没有父级，因此对于该页面，pParent==NULL。
**
** 成功时返回SQLITE_OK。如果我们看到该页面不包含格式良好的数据库页面，
** 则返回SQLITE_CORRUPT。请注意，返回SQLITE_OK并不保证页面格式良好。
** 它仅表明我们未能检测到任何损坏。
*/
static int initPage(MemPage *pPage, Pgno pgnoThis, MemPage *pParent) {
  int idx;        /* An index into pPage->u.aDisk[] */
                  /* pPage->u.aDisk[] 的索引 */
  Cell *pCell;    /* A pointer to a Cell in pPage->u.aDisk[] */
                  /* 指向 pPage->u.aDisk[] 中 Cell 的指针 */
  FreeBlk *pFBlk; /* A pointer to a free block in pPage->u.aDisk[] */
                  /* 指向 pPage->u.aDisk[] 中空闲块的指针 */
  int sz;         /* The size of a Cell in bytes */
                  /* Cell 的大小（以字节为单位） */
  int freeSpace;  /* Amount of free space on the page */
                  /* 页面上的可用空间量 */

  if (pPage->pParent) {
    assert(pPage->pParent == pParent);
    return SQLITE_OK;
  }
  if (pParent) {
    pPage->pParent = pParent;
    sqlitepager_ref(pParent);
  }
  if (pPage->isInit)
    return SQLITE_OK;
  pPage->isInit = 1;
  pPage->nCell = 0;
  freeSpace = USABLE_SPACE;
  idx = pPage->u.hdr.firstCell;
  while (idx != 0) {
    if (idx > SQLITE_PAGE_SIZE - MIN_CELL_SIZE)
      goto page_format_error;
    if (idx < sizeof(PageHdr))
      goto page_format_error;
    if (idx != ROUNDUP(idx))
      goto page_format_error;
    pCell = (Cell *)&pPage->u.aDisk[idx];
    sz = cellSize(pCell);
    if (idx + sz > SQLITE_PAGE_SIZE)
      goto page_format_error;
    freeSpace -= sz;
    pPage->apCell[pPage->nCell++] = pCell;
    idx = pCell->h.iNext;
  }
  pPage->nFree = 0;
  idx = pPage->u.hdr.firstFree;
  while (idx != 0) {
    if (idx > SQLITE_PAGE_SIZE - sizeof(FreeBlk))
      goto page_format_error;
    if (idx < sizeof(PageHdr))
      goto page_format_error;
    pFBlk = (FreeBlk *)&pPage->u.aDisk[idx];
    pPage->nFree += pFBlk->iSize;
    if (pFBlk->iNext > 0 && pFBlk->iNext <= idx)
      goto page_format_error;
    idx = pFBlk->iNext;
  }
  if (pPage->nCell == 0 && pPage->nFree == 0) {
    /* As a special case, an uninitialized root page appears to be
    ** an empty database
    **
    ** 作为特殊情况，未初始化的根页面看起来像是一个空数据库
    */
    return SQLITE_OK;
  }
  if (pPage->nFree != freeSpace)
    goto page_format_error;
  return SQLITE_OK;

page_format_error:
  return SQLITE_CORRUPT;
}

/*
** Set up a raw page so that it looks like a database page holding
** no entries.
**
** 设置原始页面，使其看起来像不包含任何条目的数据库页面。
*/
static void zeroPage(MemPage *pPage) {
  PageHdr *pHdr;
  FreeBlk *pFBlk;
  assert(sqlitepager_iswriteable(pPage));
  memset(pPage, 0, SQLITE_PAGE_SIZE);
  pHdr = &pPage->u.hdr;
  pHdr->firstCell = 0;
  pHdr->firstFree = sizeof(*pHdr);
  pFBlk = (FreeBlk *)&pHdr[1];
  pFBlk->iNext = 0;
  pFBlk->iSize = SQLITE_PAGE_SIZE - sizeof(*pHdr);
  pPage->nFree = pFBlk->iSize;
  pPage->nCell = 0;
  pPage->isOverfull = 0;
}

/*
** This routine is called when the reference count for a page
** reaches zero.  We need to unref the pParent pointer when that
** happens.
**
** 当页面的引用计数达到零时调用此例程。
** 发生这种情况时，我们需要取消引用pParent指针。
*/
static void pageDestructor(void *pData) {
  MemPage *pPage = (MemPage *)pData;
  if (pPage->pParent) {
    MemPage *pParent = pPage->pParent;
    pPage->pParent = 0;
    sqlitepager_unref(pParent);
  }
}

/*
** Open a new database.
**
** Actually, this routine just sets up the internal data structures
** for accessing the database.  We do not open the database file
** until the first page is loaded.
**
** zFilename is the name of the database file.  If zFilename is NULL
** a new database with a random name is created.  This randomly named
** database file will be deleted when sqliteBtreeClose() is called.
**
** 打开一个新的数据库。
**
** 实际上，此例程只是设置用于访问数据库的内部数据结构。
** 在加载第一页之前，我们不会打开数据库文件。
**
** zFilename是数据库文件的名称。如果zFilename为NULL，
** 则会创建一个具有随机名称的新数据库。
** 当调用sqliteBtreeClose()时，此随机命名的数据库文件将被删除。
*/
int sqliteBtreeOpen(
    const char *zFilename, /* Name of the file containing the BTree database */
                           /* 包含 BTree 数据库的文件名 */
    int mode,              /* Not currently used */
                           /* 当前未使用 */
    int nCache,            /* How many pages in the page cache */
                           /* 页面缓存中有多少页 */
    Btree **ppBtree        /* Pointer to new Btree object written here */
                           /* 指向此处写入的新 Btree 对象的指针 */
) {
  Btree *pBt;
  int rc;

  pBt = malloc(sizeof(*pBt));
  memset(pBt, 0, sizeof(*pBt));

  if (pBt == 0) {
    *ppBtree = 0;
    return SQLITE_NOMEM;
  }
  if (nCache < 10)
    nCache = 10;
  rc = sqlitepager_open(&pBt->pPager, zFilename, nCache, EXTRA_SIZE);
  if (rc != SQLITE_OK) {
    if (pBt->pPager)
      sqlitepager_close(pBt->pPager);
    sqliteFree(pBt);
    *ppBtree = 0;
    return rc;
  }
  sqlitepager_set_destructor(pBt->pPager, pageDestructor);
  pBt->pCursor = 0;
  pBt->page1 = 0;
  pBt->readOnly = sqlitepager_isreadonly(pBt->pPager);
  sqliteHashInit(&pBt->locks, SQLITE_HASH_INT, 0);
  *ppBtree = pBt;
  return SQLITE_OK;
}

/*
** Close an open database and invalidate all cursors.
**
** 关闭打开的数据库并使所有游标无效。
*/
int sqliteBtreeClose(Btree *pBt) {
  while (pBt->pCursor) {
    sqliteBtreeCloseCursor(pBt->pCursor);
  }
  sqlitepager_close(pBt->pPager);
  sqliteHashClear(&pBt->locks);
  sqliteFree(pBt);
  return SQLITE_OK;
}

/*
** Change the limit on the number of pages allowed the cache.
**
** The maximum number of cache pages is set to the absolute
** value of mxPage.  If mxPage is negative, the pager will
** operate asynchronously - it will not stop to do fsync()s
** to insure data is written to the disk surface before
** continuing.  Transactions still work if synchronous is off,
** and the database cannot be corrupted if this program
** crashes.  But if the operating system crashes or there is
** an abrupt power failure when synchronous is off, the database
** could be left in an inconsistent and unrecoverable state.
** Synchronous is on by default so database corruption is not
** normally a worry.
**
** 更改缓存允许的页面数量限制。
**
** 最大缓存页面数设置为mxPage的绝对值。如果mxPage为负数，
** 则分页器将异步运行 -
*它不会停止执行fsync()以确保在继续之前将数据写入磁盘表面。
** 如果同步关闭，事务仍然有效，并且如果此程序崩溃，数据库也不会损坏。
** 但是，如果操作系统崩溃或在同步关闭时突然断电，数据库可能会处于不一致且无法恢复的状态。
** 同步默认开启，因此通常不必担心数据库损坏。
*/
int sqliteBtreeSetCacheSize(Btree *pBt, int mxPage) {
  sqlitepager_set_cachesize(pBt->pPager, mxPage);
  return SQLITE_OK;
}

/*
** Get a reference to page1 of the database file.  This will
** also acquire a readlock on that file.
**
** SQLITE_OK is returned on success.  If the file is not a
** well-formed database file, then SQLITE_CORRUPT is returned.
** SQLITE_BUSY is returned if the database is locked.  SQLITE_NOMEM
** is returned if we run out of memory.  SQLITE_PROTOCOL is returned
** if there is a locking protocol violation.
**
** 获取对数据库文件page1的引用。这也将获取该文件的读锁。
**
** 成功时返回SQLITE_OK。如果文件不是格式良好的数据库文件，则返回SQLITE_CORRUPT。
** 如果数据库被锁定，则返回SQLITE_BUSY。如果我们内存不足，则返回SQLITE_NOMEM。
** 如果存在锁定协议冲突，则返回SQLITE_PROTOCOL。
*/
static int lockBtree(Btree *pBt) {
  int rc;
  if (pBt->page1)
    return SQLITE_OK;
  rc = sqlitepager_get(pBt->pPager, 1, (void **)&pBt->page1);
  if (rc != SQLITE_OK)
    return rc;

  /* Do some checking to help insure the file we opened really is
  ** a valid database file.
  **
  ** 做一些检查以帮助确保我们打开的文件确实是一个有效的数据库文件。
  */
  if (sqlitepager_pagecount(pBt->pPager) > 0) {
    PageOne *pP1 = pBt->page1;
    if (strcmp(pP1->zMagic, zMagicHeader) != 0 || pP1->iMagic != MAGIC) {
      rc = SQLITE_CORRUPT;
      goto page1_init_failed;
    }
  }
  return rc;

page1_init_failed:
  sqlitepager_unref(pBt->page1);
  pBt->page1 = 0;
  return rc;
}

/*
** If there are no outstanding cursors and we are not in the middle
** of a transaction but there is a read lock on the database, then
** this routine unrefs the first page of the database file which
** has the effect of releasing the read lock.
**
** If there are any outstanding cursors, this routine is a no-op.
**
** If there is a transaction in progress, this routine is a no-op.
**
** 如果没有未完成的游标，并且我们不在事务中间，但数据库上有读锁，
** 则此例程取消引用数据库文件的第一页，这具有释放读锁的效果。
**
** 如果有任何未完成的游标，此例程为无操作。
**
** 如果正在进行事务，此例程为无操作。
*/
static void unlockBtreeIfUnused(Btree *pBt) {
  if (pBt->inTrans == 0 && pBt->pCursor == 0 && pBt->page1 != 0) {
    sqlitepager_unref(pBt->page1);
    pBt->page1 = 0;
    pBt->inTrans = 0;
    pBt->inCkpt = 0;
  }
}

/*
** Create a new database by initializing the first two pages of the
** file.
**
** 通过初始化文件的前两页来创建一个新数据库。
*/
static int newDatabase(Btree *pBt) {
  MemPage *pRoot;
  PageOne *pP1;
  int rc;
  if (sqlitepager_pagecount(pBt->pPager) > 1)
    return SQLITE_OK;
  pP1 = pBt->page1;
  rc = sqlitepager_write(pBt->page1);
  if (rc)
    return rc;
  rc = sqlitepager_get(pBt->pPager, 2, (void **)&pRoot);
  if (rc)
    return rc;
  rc = sqlitepager_write(pRoot);
  if (rc) {
    sqlitepager_unref(pRoot);
    return rc;
  }
  strcpy(pP1->zMagic, zMagicHeader);
  pP1->iMagic = MAGIC;
  zeroPage(pRoot);
  sqlitepager_unref(pRoot);
  return SQLITE_OK;
}

/*
** Attempt to start a new transaction.
**
** A transaction must be started before attempting any changes
** to the database.  None of the following routines will work
** unless a transaction is started first:
**
**      sqliteBtreeCreateTable()
**      sqliteBtreeCreateIndex()
**      sqliteBtreeClearTable()
**      sqliteBtreeDropTable()
**      sqliteBtreeInsert()
**      sqliteBtreeDelete()
**      sqliteBtreeUpdateMeta()
**
** 尝试启动新事务。
**
** 在尝试对数据库进行任何更改之前，必须启动事务。
** 除非先启动事务，否则以下例程均无法工作：
**
**      sqliteBtreeCreateTable()
**      sqliteBtreeCreateIndex()
**      sqliteBtreeClearTable()
**      sqliteBtreeDropTable()
**      sqliteBtreeInsert()
**      sqliteBtreeDelete()
**      sqliteBtreeUpdateMeta()
*/
int sqliteBtreeBeginTrans(Btree *pBt) {
  int rc;
  if (pBt->inTrans)
    return SQLITE_ERROR;
  if (pBt->page1 == 0) {
    rc = lockBtree(pBt);
    if (rc != SQLITE_OK) {
      return rc;
    }
  }
  if (pBt->readOnly) {
    rc = SQLITE_OK;
  } else {
    rc = sqlitepager_begin(pBt->page1);
    if (rc == SQLITE_OK) {
      rc = newDatabase(pBt);
    }
  }
  if (rc == SQLITE_OK) {
    pBt->inTrans = 1;
    pBt->inCkpt = 0;
  } else {
    unlockBtreeIfUnused(pBt);
  }
  return rc;
}

/*
** Commit the transaction currently in progress.
**
** This will release the write lock on the database file.  If there
** are no active cursors, it also releases the read lock.
**
** 提交当前正在进行的事务。
**
** 这将释放数据库文件上的写锁。如果没有活动游标，
** 它也会释放读锁。
*/
int sqliteBtreeCommit(Btree *pBt) {
  int rc;
  if (pBt->inTrans == 0)
    return SQLITE_ERROR;
  rc = pBt->readOnly ? SQLITE_OK : sqlitepager_commit(pBt->pPager);
  pBt->inTrans = 0;
  pBt->inCkpt = 0;
  unlockBtreeIfUnused(pBt);
  return rc;
}

/*
** Rollback the transaction in progress.  All cursors will be
** invalided by this operation.  Any attempt to use a cursor
** that was open at the beginning of this operation will result
** in an error.
**
** This will release the write lock on the database file.  If there
** are no active cursors, it also releases the read lock.
**
** 回滚正在进行的事务。此操作将使所有游标无效。
** 任何尝试使用在此操作开始时打开的游标都将导致错误。
**
** 这将释放数据库文件上的写锁。如果没有活动游标，
** 它也会释放读锁。
*/
int sqliteBtreeRollback(Btree *pBt) {
  int rc;
  BtCursor *pCur;
  if (pBt->inTrans == 0)
    return SQLITE_OK;
  pBt->inTrans = 0;
  pBt->inCkpt = 0;
  for (pCur = pBt->pCursor; pCur; pCur = pCur->pNext) {
    if (pCur->pPage) {
      sqlitepager_unref(pCur->pPage);
      pCur->pPage = 0;
    }
  }
  rc = pBt->readOnly ? SQLITE_OK : sqlitepager_rollback(pBt->pPager);
  unlockBtreeIfUnused(pBt);
  return rc;
}

/*
** Set the checkpoint for the current transaction.  The checkpoint serves
** as a sub-transaction that can be rolled back independently of the
** main transaction.  You must start a transaction before starting a
** checkpoint.  The checkpoint is ended automatically if the transaction
** commits or rolls back.
**
** Only one checkpoint may be active at a time.  It is an error to try
** to start a new checkpoint if another checkpoint is already active.
**
** 为当前事务设置检查点。检查点充当可以独立于主事务回滚的子事务。
** 在启动检查点之前，必须先启动事务。
** 如果事务提交或回滚，检查点将自动结束。
**
** 一次只能有一个检查点处于活动状态。如果另一个检查点已处于活动状态，
** 则尝试启动新检查点是错误的。
*/
int sqliteBtreeBeginCkpt(Btree *pBt) {
  int rc;
  if (!pBt->inTrans || pBt->inCkpt) {
    return SQLITE_ERROR;
  }
  rc = pBt->readOnly ? SQLITE_OK : sqlitepager_ckpt_begin(pBt->pPager);
  pBt->inCkpt = 1;
  return rc;
}

/*
** Commit a checkpoint to transaction currently in progress.  If no
** checkpoint is active, this is a no-op.
**
** 将检查点提交到当前正在进行的事务。如果没有活动检查点，则此操作为无操作。
*/
int sqliteBtreeCommitCkpt(Btree *pBt) {
  int rc;
  if (pBt->inCkpt && !pBt->readOnly) {
    rc = sqlitepager_ckpt_commit(pBt->pPager);
  } else {
    rc = SQLITE_OK;
  }
  pBt->inCkpt = 0;
  return rc;
}

/*
** Rollback the checkpoint to the current transaction.  If there
** is no active checkpoint or transaction, this routine is a no-op.
**
** All cursors will be invalided by this operation.  Any attempt
** to use a cursor that was open at the beginning of this operation
** will result in an error.
**
** 将检查点回滚到当前事务。如果没有活动检查点或事务，则此例程为无操作。
**
** 此操作将使所有游标无效。任何尝试使用在此操作开始时打开的游标都将导致错误。
*/
int sqliteBtreeRollbackCkpt(Btree *pBt) {
  int rc;
  BtCursor *pCur;
  if (pBt->inCkpt == 0 || pBt->readOnly)
    return SQLITE_OK;
  for (pCur = pBt->pCursor; pCur; pCur = pCur->pNext) {
    if (pCur->pPage) {
      sqlitepager_unref(pCur->pPage);
      pCur->pPage = 0;
    }
  }
  rc = sqlitepager_ckpt_rollback(pBt->pPager);
  pBt->inCkpt = 0;
  return rc;
}

/*
** Create a new cursor for the BTree whose root is on the page
** iTable.  The act of acquiring a cursor gets a read lock on
** the database file.
**
** If wrFlag==0, then the cursor can only be used for reading.
** If wrFlag==1, then the cursor can be used for reading or writing.
** A read/write cursor requires exclusive access to its table.  There
** cannot be two or more cursors open on the same table if any one of
** cursors is a read/write cursor.  But there can be two or more
** read-only cursors open on the same table.
**
** No checking is done to make sure that page iTable really is the
** root page of a b-tree.  If it is not, then the cursor acquired
** will not work correctly.
**
** 为根位于页面iTable上的B树创建一个新游标。
** 获取游标的操作会获取数据库文件上的读锁。
**
** 如果wrFlag==0，则游标只能用于读取。
** 如果wrFlag==1，则游标可用于读取或写入。
** 读/写游标需要对其表进行独占访问。如果任何一个游标是读/写游标，
** 则同一张表上不能有两个或更多游标打开。
** 但同一张表上可以有两个或更多只读游标打开。
**
** 不会进行检查以确保页面iTable确实是b树的根页。
** 如果不是，则获取的游标将无法正常工作。
*/
int sqliteBtreeCursor(Btree *pBt, int iTable, int wrFlag, BtCursor **ppCur) {
  int rc;
  BtCursor *pCur;
  ptr nLock;

  if (pBt->page1 == 0) {
    rc = lockBtree(pBt);
    if (rc != SQLITE_OK) {
      *ppCur = 0;
      return rc;
    }
  }
  if (wrFlag && pBt->readOnly) {
    *ppCur = 0;
    return SQLITE_READONLY;
  }
  pCur = sqliteMalloc(sizeof(*pCur));
  if (pCur == 0) {
    rc = SQLITE_NOMEM;
    goto create_cursor_exception;
  }
  pCur->pgnoRoot = (Pgno)iTable;
  rc = sqlitepager_get(pBt->pPager, pCur->pgnoRoot, (void **)&pCur->pPage);
  if (rc != SQLITE_OK) {
    goto create_cursor_exception;
  }
  rc = initPage(pCur->pPage, pCur->pgnoRoot, 0);
  if (rc != SQLITE_OK) {
    goto create_cursor_exception;
  }
  nLock = (ptr)sqliteHashFind(&pBt->locks, 0, iTable);
  if (nLock < 0 || (nLock > 0 && wrFlag)) {
    rc = SQLITE_LOCKED;
    goto create_cursor_exception;
  }
  nLock = wrFlag ? -1 : nLock + 1;
  sqliteHashInsert(&pBt->locks, 0, iTable, (void *)nLock);
  pCur->pBt = pBt;
  pCur->wrFlag = wrFlag;
  pCur->idx = 0;
  pCur->pNext = pBt->pCursor;
  if (pCur->pNext) {
    pCur->pNext->pPrev = pCur;
  }
  pCur->pPrev = 0;
  pBt->pCursor = pCur;
  *ppCur = pCur;
  return SQLITE_OK;

create_cursor_exception:
  *ppCur = 0;
  if (pCur) {
    if (pCur->pPage)
      sqlitepager_unref(pCur->pPage);
    sqliteFree(pCur);
  }
  unlockBtreeIfUnused(pBt);
  return rc;
}

/*
** Close a cursor.  The read lock on the database file is released
** when the last cursor is closed.
**
** 关闭游标。当最后一个游标关闭时，释放数据库文件上的读锁。
*/
int sqliteBtreeCloseCursor(BtCursor *pCur) {
  ptr nLock;
  Btree *pBt = pCur->pBt;
  if (pCur->pPrev) {
    pCur->pPrev->pNext = pCur->pNext;
  } else {
    pBt->pCursor = pCur->pNext;
  }
  if (pCur->pNext) {
    pCur->pNext->pPrev = pCur->pPrev;
  }
  if (pCur->pPage) {
    sqlitepager_unref(pCur->pPage);
  }
  unlockBtreeIfUnused(pBt);
  nLock = (ptr)sqliteHashFind(&pBt->locks, 0, pCur->pgnoRoot);
  assert(nLock != 0 || sqlite_malloc_failed);
  nLock = nLock < 0 ? 0 : nLock - 1;
  sqliteHashInsert(&pBt->locks, 0, pCur->pgnoRoot, (void *)nLock);
  sqliteFree(pCur);
  return SQLITE_OK;
}

/*
** Make a temporary cursor by filling in the fields of pTempCur.
** The temporary cursor is not on the cursor list for the Btree.
**
** 通过填写pTempCur的字段来创建一个临时游标。
** 临时游标不在B树的游标列表中。
*/
static void getTempCursor(BtCursor *pCur, BtCursor *pTempCur) {
  memcpy(pTempCur, pCur, sizeof(*pCur));
  pTempCur->pNext = 0;
  pTempCur->pPrev = 0;
  if (pTempCur->pPage) {
    sqlitepager_ref(pTempCur->pPage);
  }
}

/*
** Delete a temporary cursor such as was made by the CreateTemporaryCursor()
** function above.
**
** 删除临时游标，例如由上面的CreateTemporaryCursor()函数创建的游标。
*/
static void releaseTempCursor(BtCursor *pCur) {
  if (pCur->pPage) {
    sqlitepager_unref(pCur->pPage);
  }
}

/*
** Set *pSize to the number of bytes of key in the entry the
** cursor currently points to.  Always return SQLITE_OK.
** Failure is not possible.  If the cursor is not currently
** pointing to an entry (which can happen, for example, if
** the database is empty) then *pSize is set to 0.
**
** 将*pSize设置为游标当前指向的条目中键的字节数。始终返回SQLITE_OK。
** 不可能失败。如果游标当前未指向条目（例如，如果数据库为空，则可能会发生这种情况），
** 则*pSize设置为0。
*/
int sqliteBtreeKeySize(BtCursor *pCur, int *pSize) {
  Cell *pCell;
  MemPage *pPage;

  pPage = pCur->pPage;
  if (pPage == 0 || pCur->idx >= pPage->nCell) {
    *pSize = 0;
  } else {
    pCell = pPage->apCell[pCur->idx];
    *pSize = NKEY(pCell->h);
  }
  return SQLITE_OK;
}

/*
** Read payload information from the entry that the pCur cursor is
** pointing to.  Begin reading the payload at "offset" and read
** a total of "amt" bytes.  Put the result in zBuf.
**
** This routine does not make a distinction between key and data.
** It just reads bytes from the payload area.
**
** 从pCur游标指向的条目读取有效载荷信息。
** 从“offset”开始读取有效载荷，共读取“amt”个字节。将结果放入zBuf中。
**
** 此例程不区分键和数据。它只是从有效载荷区域读取字节。
*/
static int getPayload(BtCursor *pCur, int offset, int amt, char *zBuf) {
  char *aPayload;
  Pgno nextPage;
  int rc;
  assert(pCur != 0 && pCur->pPage != 0);
  assert(pCur->idx >= 0 && pCur->idx < pCur->pPage->nCell);
  aPayload = pCur->pPage->apCell[pCur->idx]->aPayload;
  if (offset < MX_LOCAL_PAYLOAD) {
    int a = amt;
    if (a + offset > MX_LOCAL_PAYLOAD) {
      a = MX_LOCAL_PAYLOAD - offset;
    }
    memcpy(zBuf, &aPayload[offset], a);
    if (a == amt) {
      return SQLITE_OK;
    }
    offset = 0;
    zBuf += a;
    amt -= a;
  } else {
    offset -= MX_LOCAL_PAYLOAD;
  }
  if (amt > 0) {
    nextPage = pCur->pPage->apCell[pCur->idx]->ovfl;
  }
  while (amt > 0 && nextPage) {
    OverflowPage *pOvfl;
    rc = sqlitepager_get(pCur->pBt->pPager, nextPage, (void **)&pOvfl);
    if (rc != 0) {
      return rc;
    }
    nextPage = pOvfl->iNext;
    if (offset < OVERFLOW_SIZE) {
      int a = amt;
      if (a + offset > OVERFLOW_SIZE) {
        a = OVERFLOW_SIZE - offset;
      }
      memcpy(zBuf, &pOvfl->aPayload[offset], a);
      offset = 0;
      amt -= a;
      zBuf += a;
    } else {
      offset -= OVERFLOW_SIZE;
    }
    sqlitepager_unref(pOvfl);
  }
  if (amt > 0) {
    return SQLITE_CORRUPT;
  }
  return SQLITE_OK;
}

/*
** Read part of the key associated with cursor pCur.  A maximum
** of "amt" bytes will be transfered into zBuf[].  The transfer
** begins at "offset".  The number of bytes actually read is
** returned.  The amount returned will be smaller than the
** amount requested if there are not enough bytes in the key
** to satisfy the request.
**
** 读取与游标pCur关联的部分键。最多将“amt”个字节传输到zBuf[]中。
** 传输从“offset”开始。返回实际读取的字节数。
** 如果键中没有足够的字节来满足请求，则返回的数量将小于请求的数量。
*/
int sqliteBtreeKey(BtCursor *pCur, int offset, int amt, char *zBuf) {
  Cell *pCell;
  MemPage *pPage;

  if (amt < 0)
    return 0;
  if (offset < 0)
    return 0;
  if (amt == 0)
    return 0;
  pPage = pCur->pPage;
  if (pPage == 0)
    return 0;
  if (pCur->idx >= pPage->nCell) {
    return 0;
  }
  pCell = pPage->apCell[pCur->idx];
  if (amt + offset > NKEY(pCell->h)) {
    amt = NKEY(pCell->h) - offset;
    if (amt <= 0) {
      return 0;
    }
  }
  getPayload(pCur, offset, amt, zBuf);
  return amt;
}

/*
** Set *pSize to the number of bytes of data in the entry the
** cursor currently points to.  Always return SQLITE_OK.
** Failure is not possible.  If the cursor is not currently
** pointing to an entry (which can happen, for example, if
** the database is empty) then *pSize is set to 0.
**
** 将*pSize设置为游标当前指向的条目中数据的字节数。始终返回SQLITE_OK。
** 不可能失败。如果游标当前未指向条目（例如，如果数据库为空，则可能会发生这种情况），
** 则*pSize设置为0。
*/
int sqliteBtreeDataSize(BtCursor *pCur, int *pSize) {
  Cell *pCell;
  MemPage *pPage;

  pPage = pCur->pPage;
  if (pPage == 0 || pCur->idx >= pPage->nCell) {
    *pSize = 0;
  } else {
    pCell = pPage->apCell[pCur->idx];
    *pSize = NDATA(pCell->h);
  }
  return SQLITE_OK;
}

/*
** Read part of the data associated with cursor pCur.  A maximum
** of "amt" bytes will be transfered into zBuf[].  The transfer
** begins at "offset".  The number of bytes actually read is
** returned.  The amount returned will be smaller than the
** amount requested if there are not enough bytes in the data
** to satisfy the request.
**
** 读取与游标pCur关联的部分数据。最多将“amt”个字节传输到zBuf[]中。
** 传输从“offset”开始。返回实际读取的字节数。
** 如果数据中没有足够的字节来满足请求，则返回的数量将小于请求的数量。
*/
int sqliteBtreeData(BtCursor *pCur, int offset, int amt, char *zBuf) {
  Cell *pCell;
  MemPage *pPage;

  if (amt < 0)
    return 0;
  if (offset < 0)
    return 0;
  if (amt == 0)
    return 0;
  pPage = pCur->pPage;
  if (pPage == 0 || pCur->idx >= pPage->nCell) {
    return 0;
  }
  pCell = pPage->apCell[pCur->idx];
  if (amt + offset > NDATA(pCell->h)) {
    amt = NDATA(pCell->h) - offset;
    if (amt <= 0) {
      return 0;
    }
  }
  getPayload(pCur, offset + NKEY(pCell->h), amt, zBuf);
  return amt;
}

/*
** Compare an external key against the key on the entry that pCur points to.
**
** The external key is pKey and is nKey bytes long.  The last nIgnore bytes
** of the key associated with pCur are ignored, as if they do not exist.
** (The normal case is for nIgnore to be zero in which case the entire
** internal key is used in the comparison.)
**
** The comparison result is written to *pRes as follows:
**
**    *pRes<0    This means pCur<pKey
**
**    *pRes==0   This means pCur==pKey for all nKey bytes
**
**    *pRes>0    This means pCur>pKey
**
** When one key is an exact prefix of the other, the shorter key is
** considered less than the longer one.  In order to be equal the
** keys must be exactly the same length. (The length of the pCur key
** is the actual key length minus nIgnore bytes.)
**
** 将外部键与pCur指向的条目上的键进行比较。
**
** 外部键是pKey，长度为nKey字节。与pCur关联的键的最后nIgnore字节被忽略，
** 就像它们不存在一样。（通常情况下nIgnore为零，此时比较中使用整个内部键。）
**
** 比较结果写入*pRes，如下所示：
**
**    *pRes<0    这意味着pCur<pKey
**
**    *pRes==0   这意味着对于所有nKey字节，pCur==pKey
**
**    *pRes>0    这意味着pCur>pKey
**
** 当一个键是另一个键的精确前缀时，较短的键被认为小于较长的键。
** 为了相等，键的长度必须完全相同。（pCur键的长度是实际键长度减去nIgnore字节。）
*/
int sqliteBtreeKeyCompare(
    BtCursor *pCur,   /* Pointer to entry to compare against */
                      /* 指向要比较的条目的指针 */
    const void *pKey, /* Key to compare against entry that pCur points to */
                      /* 与 pCur 指向的条目进行比较的键 */
    int nKey,         /* Number of bytes in pKey */
                      /* pKey 中的字节数 */
    int nIgnore,      /* Ignore this many bytes at the end of pCur */
                      /* 忽略 pCur 末尾的这么多字节 */
    int *pResult      /* Write the result here */
                      /* 将结果写在这里 */
) {
  Pgno nextPage;
  int n, c, rc, nLocal;
  Cell *pCell;
  const char *zKey = (const char *)pKey;

  assert(pCur->pPage);
  assert(pCur->idx >= 0 && pCur->idx < pCur->pPage->nCell);
  pCell = pCur->pPage->apCell[pCur->idx];
  nLocal = NKEY(pCell->h) - nIgnore;
  if (nLocal < 0)
    nLocal = 0;
  n = nKey < nLocal ? nKey : nLocal;
  if (n > MX_LOCAL_PAYLOAD) {
    n = MX_LOCAL_PAYLOAD;
  }
  c = memcmp(pCell->aPayload, zKey, n);
  if (c != 0) {
    *pResult = c;
    return SQLITE_OK;
  }
  zKey += n;
  nKey -= n;
  nLocal -= n;
  nextPage = pCell->ovfl;
  while (nKey > 0 && nLocal > 0) {
    OverflowPage *pOvfl;
    if (nextPage == 0) {
      return SQLITE_CORRUPT;
    }
    rc = sqlitepager_get(pCur->pBt->pPager, nextPage, (void **)&pOvfl);
    if (rc) {
      return rc;
    }
    nextPage = pOvfl->iNext;
    n = nKey < nLocal ? nKey : nLocal;
    if (n > OVERFLOW_SIZE) {
      n = OVERFLOW_SIZE;
    }
    c = memcmp(pOvfl->aPayload, zKey, n);
    sqlitepager_unref(pOvfl);
    if (c != 0) {
      *pResult = c;
      return SQLITE_OK;
    }
    nKey -= n;
    nLocal -= n;
    zKey += n;
  }
  if (c == 0) {
    c = nLocal - nKey;
  }
  *pResult = c;
  return SQLITE_OK;
}

/*
** Move the cursor down to a new child page.
**
** 将游标向下移动到新的子页面。
*/
static int moveToChild(BtCursor *pCur, int newPgno) {
  int rc;
  MemPage *pNewPage;

  rc = sqlitepager_get(pCur->pBt->pPager, newPgno, (void **)&pNewPage);
  if (rc)
    return rc;
  rc = initPage(pNewPage, newPgno, pCur->pPage);
  if (rc)
    return rc;
  sqlitepager_unref(pCur->pPage);
  pCur->pPage = pNewPage;
  pCur->idx = 0;
  return SQLITE_OK;
}

/*
** Move the cursor up to the parent page.
**
** pCur->idx is set to the cell index that contains the pointer
** to the page we are coming from.  If we are coming from the
** right-most child page then pCur->idx is set to one more than
** the largest cell index.
**
** 将游标向上移动到父页面。
**
** pCur->idx设置为包含指向我们来源页面的指针的单元索引。
** 如果我们来自最右侧的子页面，则pCur->idx设置为比最大单元索引大一。
*/
static int moveToParent(BtCursor *pCur) {
  Pgno oldPgno;
  MemPage *pParent;
  int i;
  pParent = pCur->pPage->pParent;
  if (pParent == 0)
    return SQLITE_INTERNAL;
  oldPgno = sqlitepager_pagenumber(pCur->pPage);
  sqlitepager_ref(pParent);
  sqlitepager_unref(pCur->pPage);
  pCur->pPage = pParent;
  pCur->idx = pParent->nCell;
  for (i = 0; i < pParent->nCell; i++) {
    if (pParent->apCell[i]->h.leftChild == oldPgno) {
      pCur->idx = i;
      break;
    }
  }
  return SQLITE_OK;
}

/*
** Move the cursor to the root page
**
** 将游标移动到根页面
*/
static int moveToRoot(BtCursor *pCur) {
  MemPage *pNew;
  int rc;

  rc = sqlitepager_get(pCur->pBt->pPager, pCur->pgnoRoot, (void **)&pNew);
  if (rc)
    return rc;
  rc = initPage(pNew, pCur->pgnoRoot, 0);
  if (rc)
    return rc;
  sqlitepager_unref(pCur->pPage);
  pCur->pPage = pNew;
  pCur->idx = 0;
  return SQLITE_OK;
}

/*
** Move the cursor down to the left-most leaf entry beneath the
** entry to which it is currently pointing.
**
** 将游标向下移动到其当前指向的条目下方的最左侧叶条目。
*/
static int moveToLeftmost(BtCursor *pCur) {
  Pgno pgno;
  int rc;

  while ((pgno = pCur->pPage->apCell[pCur->idx]->h.leftChild) != 0) {
    rc = moveToChild(pCur, pgno);
    if (rc)
      return rc;
  }
  return SQLITE_OK;
}

/* Move the cursor to the first entry in the table.  Return SQLITE_OK
** on success.  Set *pRes to 0 if the cursor actually points to something
** or set *pRes to 1 if the table is empty.
**
** 将游标移动到表中的第一个条目。成功时返回SQLITE_OK。
** 如果游标实际上指向某物，则将*pRes设置为0；如果表为空，则将*pRes设置为1。
*/
int sqliteBtreeFirst(BtCursor *pCur, int *pRes) {
  int rc;
  if (pCur->pPage == 0)
    return SQLITE_ABORT;
  rc = moveToRoot(pCur);
  if (rc)
    return rc;
  if (pCur->pPage->nCell == 0) {
    *pRes = 1;
    return SQLITE_OK;
  }
  *pRes = 0;
  rc = moveToLeftmost(pCur);
  pCur->bSkipNext = 0;
  return rc;
}

/* Move the cursor to the last entry in the table.  Return SQLITE_OK
** on success.  Set *pRes to 0 if the cursor actually points to something
** or set *pRes to 1 if the table is empty.
**
** 将游标移动到表中的最后一个条目。成功时返回SQLITE_OK。
** 如果游标实际上指向某物，则将*pRes设置为0；如果表为空，则将*pRes设置为1。
*/
int sqliteBtreeLast(BtCursor *pCur, int *pRes) {
  int rc;
  Pgno pgno;
  if (pCur->pPage == 0)
    return SQLITE_ABORT;
  rc = moveToRoot(pCur);
  if (rc)
    return rc;
  if (pCur->pPage->nCell == 0) {
    *pRes = 1;
    return SQLITE_OK;
  }
  *pRes = 0;
  while ((pgno = pCur->pPage->u.hdr.rightChild) != 0) {
    rc = moveToChild(pCur, pgno);
    if (rc)
      return rc;
  }
  pCur->idx = pCur->pPage->nCell - 1;
  pCur->bSkipNext = 0;
  return rc;
}

/* Move the cursor so that it points to an entry near pKey.
** Return a success code.
**
** If an exact match is not found, then the cursor is always
** left pointing at a leaf page which would hold the entry if it
** were present.  The cursor might point to an entry that comes
** before or after the key.
**
** The result of comparing the key with the entry to which the
** cursor is left pointing is stored in pCur->iMatch.  The same
** value is also written to *pRes if pRes!=NULL.  The meaning of
** this value is as follows:
**
**     *pRes<0      The cursor is left pointing at an entry that
**                  is smaller than pKey.
**
**     *pRes==0     The cursor is left pointing at an entry that
**                  exactly matches pKey.
**
**     *pRes>0      The cursor is left pointing at an entry that
**                  is larger than pKey.
**
** 移动游标，使其指向pKey附近的条目。返回成功代码。
**
** 如果未找到完全匹配项，则游标始终指向叶页面，如果该条目存在，
** 则该叶页面将保存该条目。游标可能指向键之前或之后的条目。
**
** 将键与游标指向的条目进行比较的结果存储在pCur->iMatch中。
** 如果pRes!=NULL，相同的值也会写入*pRes。此值的含义如下：
**
**     *pRes<0      游标指向小于pKey的条目。
**
**     *pRes==0     游标指向与pKey完全匹配的条目。
**
**     *pRes>0      游标指向大于pKey的条目。
*/
int sqliteBtreeMoveto(BtCursor *pCur, const void *pKey, int nKey, int *pRes) {
  int rc;
  if (pCur->pPage == 0)
    return SQLITE_ABORT;
  pCur->bSkipNext = 0;
  rc = moveToRoot(pCur);
  if (rc)
    return rc;
  for (;;) {
    int lwr, upr;
    Pgno chldPg;
    MemPage *pPage = pCur->pPage;
    int c = -1;
    lwr = 0;
    upr = pPage->nCell - 1;
    while (lwr <= upr) {
      pCur->idx = (lwr + upr) / 2;
      rc = sqliteBtreeKeyCompare(pCur, pKey, nKey, 0, &c);
      if (rc)
        return rc;
      if (c == 0) {
        pCur->iMatch = c;
        if (pRes)
          *pRes = 0;
        return SQLITE_OK;
      }
      if (c < 0) {
        lwr = pCur->idx + 1;
      } else {
        upr = pCur->idx - 1;
      }
    }
    assert(lwr == upr + 1);
    if (lwr >= pPage->nCell) {
      chldPg = pPage->u.hdr.rightChild;
    } else {
      chldPg = pPage->apCell[lwr]->h.leftChild;
    }
    if (chldPg == 0) {
      pCur->iMatch = c;
      if (pRes)
        *pRes = c;
      return SQLITE_OK;
    }
    rc = moveToChild(pCur, chldPg);
    if (rc)
      return rc;
  }
  /* NOT REACHED */
}

/*
** Advance the cursor to the next entry in the database.  If
** successful and pRes!=NULL then set *pRes=0.  If the cursor
** was already pointing to the last entry in the database before
** this routine was called, then set *pRes=1 if pRes!=NULL.
**
** 将游标前进到数据库中的下一个条目。如果成功且pRes!=NULL，
** 则设置*pRes=0。如果在此例程调用之前游标已经指向数据库中的最后一个条目，
** 且pRes!=NULL，则设置*pRes=1。
*/
int sqliteBtreeNext(BtCursor *pCur, int *pRes) {
  int rc;
  if (pCur->pPage == 0) {
    if (pRes)
      *pRes = 1;
    return SQLITE_ABORT;
  }
  if (pCur->bSkipNext && pCur->idx < pCur->pPage->nCell) {
    pCur->bSkipNext = 0;
    if (pRes)
      *pRes = 0;
    return SQLITE_OK;
  }
  pCur->idx++;
  if (pCur->idx >= pCur->pPage->nCell) {
    if (pCur->pPage->u.hdr.rightChild) {
      rc = moveToChild(pCur, pCur->pPage->u.hdr.rightChild);
      if (rc)
        return rc;
      rc = moveToLeftmost(pCur);
      if (rc)
        return rc;
      if (pRes)
        *pRes = 0;
      return SQLITE_OK;
    }
    do {
      if (pCur->pPage->pParent == 0) {
        if (pRes)
          *pRes = 1;
        return SQLITE_OK;
      }
      rc = moveToParent(pCur);
      if (rc)
        return rc;
    } while (pCur->idx >= pCur->pPage->nCell);
    if (pRes)
      *pRes = 0;
    return SQLITE_OK;
  }
  rc = moveToLeftmost(pCur);
  if (rc)
    return rc;
  if (pRes)
    *pRes = 0;
  return SQLITE_OK;
}

/*
** Allocate a new page from the database file.
**
** The new page is marked as dirty.  (In other words, sqlitepager_write()
** has already been called on the new page.)  The new page has also
** been referenced and the calling routine is responsible for calling
** sqlitepager_unref() on the new page when it is done.
**
** SQLITE_OK is returned on success.  Any other return value indicates
** an error.  *ppPage and *pPgno are undefined in the event of an error.
** Do not invoke sqlitepager_unref() on *ppPage if an error is returned.
**
** 从数据库文件分配一个新页面。
**
** 新页面被标记为脏。（换句话说，已经在新页面上调用了sqlitepager_write()。）
** 新页面也已被引用，调用例程负责在完成后对新页面调用sqlitepager_unref()。
**
** 成功时返回SQLITE_OK。任何其他返回值都表示错误。
** 如果发生错误，*ppPage和*pPgno未定义。
** 如果返回错误，请勿对*ppPage调用sqlitepager_unref()。
*/
static int allocatePage(Btree *pBt, MemPage **ppPage, Pgno *pPgno) {
  PageOne *pPage1 = pBt->page1;
  int rc;
  if (pPage1->freeList) {
    OverflowPage *pOvfl;
    FreelistInfo *pInfo;

    rc = sqlitepager_write(pPage1);
    if (rc)
      return rc;
    pPage1->nFree--;
    rc = sqlitepager_get(pBt->pPager, pPage1->freeList, (void **)&pOvfl);
    if (rc)
      return rc;
    rc = sqlitepager_write(pOvfl);
    if (rc) {
      sqlitepager_unref(pOvfl);
      return rc;
    }
    pInfo = (FreelistInfo *)pOvfl->aPayload;
    if (pInfo->nFree == 0) {
      *pPgno = pPage1->freeList;
      pPage1->freeList = pOvfl->iNext;
      *ppPage = (MemPage *)pOvfl;
    } else {
      pInfo->nFree--;
      *pPgno = pInfo->aFree[pInfo->nFree];
      rc = sqlitepager_get(pBt->pPager, *pPgno, (void **)ppPage);
      sqlitepager_unref(pOvfl);
      if (rc == SQLITE_OK) {
        sqlitepager_dont_rollback(*ppPage);
        rc = sqlitepager_write(*ppPage);
      }
    }
  } else {
    *pPgno = sqlitepager_pagecount(pBt->pPager) + 1;
    rc = sqlitepager_get(pBt->pPager, *pPgno, (void **)ppPage);
    if (rc)
      return rc;
    rc = sqlitepager_write(*ppPage);
  }
  return rc;
}

/*
** Add a page of the database file to the freelist.  Either pgno or
** pPage but not both may be 0.
**
** sqlitepager_unref() is NOT called for pPage.
**
** 将数据库文件的一个页面添加到空闲列表。pgno或pPage可以为0，但不能同时为0。
**
** 不会对pPage调用sqlitepager_unref()。
*/
static int freePage(Btree *pBt, void *pPage, Pgno pgno) {
  PageOne *pPage1 = pBt->page1;
  OverflowPage *pOvfl = (OverflowPage *)pPage;
  int rc;
  int needUnref = 0;
  MemPage *pMemPage;

  if (pgno == 0) {
    assert(pOvfl != 0);
    pgno = sqlitepager_pagenumber(pOvfl);
  }
  assert(pgno > 2);
  rc = sqlitepager_write(pPage1);
  if (rc) {
    return rc;
  }
  pPage1->nFree++;
  if (pPage1->nFree > 0 && pPage1->freeList) {
    OverflowPage *pFreeIdx;
    rc = sqlitepager_get(pBt->pPager, pPage1->freeList, (void **)&pFreeIdx);
    if (rc == SQLITE_OK) {
      FreelistInfo *pInfo = (FreelistInfo *)pFreeIdx->aPayload;
      if (pInfo->nFree < (sizeof(pInfo->aFree) / sizeof(pInfo->aFree[0]))) {
        rc = sqlitepager_write(pFreeIdx);
        if (rc == SQLITE_OK) {
          pInfo->aFree[pInfo->nFree] = pgno;
          pInfo->nFree++;
          sqlitepager_unref(pFreeIdx);
          sqlitepager_dont_write(pBt->pPager, pgno);
          return rc;
        }
      }
      sqlitepager_unref(pFreeIdx);
    }
  }
  if (pOvfl == 0) {
    assert(pgno > 0);
    rc = sqlitepager_get(pBt->pPager, pgno, (void **)&pOvfl);
    if (rc)
      return rc;
    needUnref = 1;
  }
  rc = sqlitepager_write(pOvfl);
  if (rc) {
    if (needUnref)
      sqlitepager_unref(pOvfl);
    return rc;
  }
  pOvfl->iNext = pPage1->freeList;
  pPage1->freeList = pgno;
  memset(pOvfl->aPayload, 0, OVERFLOW_SIZE);
  pMemPage = (MemPage *)pPage;
  pMemPage->isInit = 0;
  if (pMemPage->pParent) {
    sqlitepager_unref(pMemPage->pParent);
    pMemPage->pParent = 0;
  }
  if (needUnref)
    rc = sqlitepager_unref(pOvfl);
  return rc;
}

/*
** Erase all the data out of a cell.  This involves returning overflow
** pages back the freelist.
**
** 清除单元中的所有数据。这涉及将溢出页返回到空闲列表。
*/
static int clearCell(Btree *pBt, Cell *pCell) {
  Pager *pPager = pBt->pPager;
  OverflowPage *pOvfl;
  Pgno ovfl, nextOvfl;
  int rc;

  if (NKEY(pCell->h) + NDATA(pCell->h) <= MX_LOCAL_PAYLOAD) {
    return SQLITE_OK;
  }
  ovfl = pCell->ovfl;
  pCell->ovfl = 0;
  while (ovfl) {
    rc = sqlitepager_get(pPager, ovfl, (void **)&pOvfl);
    if (rc)
      return rc;
    nextOvfl = pOvfl->iNext;
    rc = freePage(pBt, pOvfl, ovfl);
    if (rc)
      return rc;
    sqlitepager_unref(pOvfl);
    ovfl = nextOvfl;
  }
  return SQLITE_OK;
}

/*
** Create a new cell from key and data.  Overflow pages are allocated as
** necessary and linked to this cell.
**
** 根据键和数据创建一个新单元。根据需要分配溢出页并将其链接到此单元。
*/
static int
fillInCell(Btree *pBt,  /* The whole Btree.  Needed to allocate pages */
                        /* 整个 Btree。需要分配页面 */
           Cell *pCell, /* Populate this Cell structure */
                        /* 填充此 Cell 结构 */
           const void *pKey, int nKey,  /* The key */
                                        /* 键 */
           const void *pData, int nData /* The data */
                                        /* 数据 */
) {
  OverflowPage *pOvfl, *pPrior;
  Pgno *pNext;
  int spaceLeft;
  int n, rc;
  int nPayload;
  const char *pPayload;
  char *pSpace;

  pCell->h.leftChild = 0;
  pCell->h.nKey = nKey & 0xffff;
  pCell->h.nKeyHi = nKey >> 16;
  pCell->h.nData = nData & 0xffff;
  pCell->h.nDataHi = nData >> 16;
  pCell->h.iNext = 0;

  pNext = &pCell->ovfl;
  pSpace = pCell->aPayload;
  spaceLeft = MX_LOCAL_PAYLOAD;
  pPayload = pKey;
  pKey = 0;
  nPayload = nKey;
  pPrior = 0;
  while (nPayload > 0) {
    if (spaceLeft == 0) {
      rc = allocatePage(pBt, (MemPage **)&pOvfl, pNext);
      if (rc) {
        *pNext = 0;
      }
      if (pPrior)
        sqlitepager_unref(pPrior);
      if (rc) {
        clearCell(pBt, pCell);
        return rc;
      }
      pPrior = pOvfl;
      spaceLeft = OVERFLOW_SIZE;
      pSpace = pOvfl->aPayload;
      pNext = &pOvfl->iNext;
    }
    n = nPayload;
    if (n > spaceLeft)
      n = spaceLeft;
    memcpy(pSpace, pPayload, n);
    nPayload -= n;
    if (nPayload == 0 && pData) {
      pPayload = pData;
      nPayload = nData;
      pData = 0;
    } else {
      pPayload += n;
    }
    spaceLeft -= n;
    pSpace += n;
  }
  *pNext = 0;
  if (pPrior) {
    sqlitepager_unref(pPrior);
  }
  return SQLITE_OK;
}

/*
** Change the MemPage.pParent pointer on the page whose number is
** given in the second argument so that MemPage.pParent holds the
** pointer in the third argument.
**
** 更改第二个参数中给出的页码的页面上的MemPage.pParent指针，
** 以便MemPage.pParent保存第三个参数中的指针。
*/
static void reparentPage(Pager *pPager, Pgno pgno, MemPage *pNewParent) {
  MemPage *pThis;

  if (pgno == 0)
    return;
  assert(pPager != 0);
  pThis = sqlitepager_lookup(pPager, pgno);
  if (pThis && pThis->isInit) {
    if (pThis->pParent != pNewParent) {
      if (pThis->pParent)
        sqlitepager_unref(pThis->pParent);
      pThis->pParent = pNewParent;
      if (pNewParent)
        sqlitepager_ref(pNewParent);
    }
    sqlitepager_unref(pThis);
  }
}

/*
** Reparent all children of the given page to be the given page.
** In other words, for every child of pPage, invoke reparentPage()
** to make sure that each child knows that pPage is its parent.
**
** This routine gets called after you memcpy() one page into
** another.
**
** 将给定页面的所有子页面的父级重置为给定页面。
** 换句话说，对于pPage的每个子页面，调用reparentPage()
** 以确保每个子页面都知道pPage是其父页面。
**
** 在将一个页面memcpy()到另一个页面后调用此例程。
*/
static void reparentChildPages(Pager *pPager, MemPage *pPage) {
  int i;
  for (i = 0; i < pPage->nCell; i++) {
    reparentPage(pPager, pPage->apCell[i]->h.leftChild, pPage);
  }
  reparentPage(pPager, pPage->u.hdr.rightChild, pPage);
}

/*
** Remove the i-th cell from pPage.  This routine effects pPage only.
** The cell content is not freed or deallocated.  It is assumed that
** the cell content has been copied someplace else.  This routine just
** removes the reference to the cell from pPage.
**
** "sz" must be the number of bytes in the cell.
**
** Do not bother maintaining the integrity of the linked list of Cells.
** Only the pPage->apCell[] array is important.  The relinkCellList()
** routine will be called soon after this routine in order to rebuild
** the linked list.
**
** 从pPage中删除第i个单元。此例程仅影响pPage。
** 单元内容未释放或取消分配。假设单元内容已复制到其他位置。
** 此例程仅从pPage中删除对单元的引用。
**
** “sz”必须是单元中的字节数。
**
** 不必费心维护Cell链表的完整性。只有pPage->apCell[]数组很重要。
** relinkCellList()例程将在此例程之后不久调用，以重建链表。
*/
static void dropCell(MemPage *pPage, int idx, int sz) {
  int j;
  assert(idx >= 0 && idx < pPage->nCell);
  assert(sz == cellSize(pPage->apCell[idx]));
  assert(sqlitepager_iswriteable(pPage));
  freeSpace(pPage, Addr(pPage->apCell[idx]) - Addr(pPage), sz);
  for (j = idx; j < pPage->nCell - 1; j++) {
    pPage->apCell[j] = pPage->apCell[j + 1];
  }
  pPage->nCell--;
}

/*
** Insert a new cell on pPage at cell index "i".  pCell points to the
** content of the cell.
**
** If the cell content will fit on the page, then put it there.  If it
** will not fit, then just make pPage->apCell[i] point to the content
** and set pPage->isOverfull.
**
** Do not bother maintaining the integrity of the linked list of Cells.
** Only the pPage->apCell[] array is important.  The relinkCellList()
** routine will be called soon after this routine in order to rebuild
** the linked list.
**
** 在pPage上的单元索引“i”处插入一个新单元。pCell指向单元的内容。
**
** 如果单元内容适合页面，则将其放在那里。如果不适合，
** 则只需使pPage->apCell[i]指向内容并设置pPage->isOverfull。
**
** 不必费心维护Cell链表的完整性。只有pPage->apCell[]数组很重要。
** relinkCellList()例程将在此例程之后不久调用，以重建链表。
*/
static void insertCell(MemPage *pPage, int i, Cell *pCell, int sz) {
  int idx, j;
  assert(i >= 0 && i <= pPage->nCell);
  assert(sz == cellSize(pCell));
  assert(sqlitepager_iswriteable(pPage));
  idx = allocateSpace(pPage, sz);
  for (j = pPage->nCell; j > i; j--) {
    pPage->apCell[j] = pPage->apCell[j - 1];
  }
  pPage->nCell++;
  if (idx <= 0) {
    pPage->isOverfull = 1;
    pPage->apCell[i] = pCell;
  } else {
    memcpy(&pPage->u.aDisk[idx], pCell, sz);
    pPage->apCell[i] = (Cell *)&pPage->u.aDisk[idx];
  }
}

/*
** Rebuild the linked list of cells on a page so that the cells
** occur in the order specified by the pPage->apCell[] array.
** Invoke this routine once to repair damage after one or more
** invocations of either insertCell() or dropCell().
**
** 重建页面上的单元链表，使单元按pPage->apCell[]数组指定的顺序出现。
** 在一次或多次调用insertCell()或dropCell()之后，调用此例程一次以修复损坏。
*/
static void relinkCellList(MemPage *pPage) {
  int i;
  u16 *pIdx;
  assert(sqlitepager_iswriteable(pPage));
  pIdx = &pPage->u.hdr.firstCell;
  for (i = 0; i < pPage->nCell; i++) {
    int idx = Addr(pPage->apCell[i]) - Addr(pPage);
    assert(idx > 0 && idx < SQLITE_PAGE_SIZE);
    *pIdx = idx;
    pIdx = &pPage->apCell[i]->h.iNext;
  }
  *pIdx = 0;
}

/*
** Make a copy of the contents of pFrom into pTo.  The pFrom->apCell[]
** pointers that point into pFrom->u.aDisk[] must be adjusted to point
** into pTo->u.aDisk[] instead.  But some pFrom->apCell[] entries might
** not point to pFrom->u.aDisk[].  Those are unchanged.
**
** 将pFrom的内容复制到pTo中。指向pFrom->u.aDisk[]的pFrom->apCell[]指针
** 必须调整为指向pTo->u.aDisk[]。但某些pFrom->apCell[]条目可能
** 不指向pFrom->u.aDisk[]。这些保持不变。
*/
static void copyPage(MemPage *pTo, MemPage *pFrom) {
  uptr from, to;
  int i;
  memcpy(pTo->u.aDisk, pFrom->u.aDisk, SQLITE_PAGE_SIZE);
  pTo->pParent = 0;
  pTo->isInit = 1;
  pTo->nCell = pFrom->nCell;
  pTo->nFree = pFrom->nFree;
  pTo->isOverfull = pFrom->isOverfull;
  to = Addr(pTo);
  from = Addr(pFrom);
  for (i = 0; i < pTo->nCell; i++) {
    uptr x = Addr(pFrom->apCell[i]);
    if (x > from && x < from + SQLITE_PAGE_SIZE) {
      *((uptr *)&pTo->apCell[i]) = x + to - from;
    } else {
      pTo->apCell[i] = pFrom->apCell[i];
    }
  }
}

/*
** This routine redistributes Cells on pPage and up to two siblings
** of pPage so that all pages have about the same amount of free space.
** Usually one sibling on either side of pPage is used in the balancing,
** though both siblings might come from one side if pPage is the first
** or last child of its parent.  If pPage has fewer than two siblings
** (something which can only happen if pPage is the root page or a
** child of root) then all available siblings participate in the balancing.
**
** The number of siblings of pPage might be increased or decreased by
** one in an effort to keep pages between 66% and 100% full. The root page
** is special and is allowed to be less than 66% full. If pPage is
** the root page, then the depth of the tree might be increased
** or decreased by one, as necessary, to keep the root page from being
** overfull or empty.
**
** This routine calls relinkCellList() on its input page regardless of
** whether or not it does any real balancing.  Client routines will typically
** invoke insertCell() or dropCell() before calling this routine, so we
** need to call relinkCellList() to clean up the mess that those other
** routines left behind.
**
** pCur is left pointing to the same cell as when this routine was called
** even if that cell gets moved to a different page.  pCur may be NULL.
** Set the pCur parameter to NULL if you do not care about keeping track
** of a cell as that will save this routine the work of keeping track of it.
**
** Note that when this routine is called, some of the Cells on pPage
** might not actually be stored in pPage->u.aDisk[].  This can happen
** if the page is overfull.  Part of the job of this routine is to
** make sure all Cells for pPage once again fit in pPage->u.aDisk[].
**
** In the course of balancing the siblings of pPage, the parent of pPage
** might become overfull or underfull.  If that happens, then this routine
** is called recursively on the parent.
**
** If this routine fails for any reason, it might leave the database
** in a corrupted state.  So if this routine fails, the database should
** be rolled back.
**
** 此例程在pPage和pPage的最多两个兄弟页面上重新分配单元，以便所有页面
** 具有大约相同数量的可用空间。通常在平衡中使用pPage两侧的一个兄弟页面，
** 尽管如果pPage是其父页面的第一个或最后一个子页面，则两个兄弟页面可能都来自一侧。
** 如果pPage的兄弟页面少于两个（只有当pPage是根页面或根的子页面时才会发生这种情况），
** 则所有可用的兄弟页面都参与平衡。
**
** pPage的兄弟页面数量可能会增加或减少一个，以保持页面在66%到100%满之间。
** 根页面很特殊，允许少于66%满。如果pPage是根页面，则树的深度可能会根据需要
** 增加或减少一个，以防止根页面过满或为空。
**
** 无论是否进行任何实际平衡，此例程都会在其输入页面上调用relinkCellList()。
** 客户端例程通常会在调用此例程之前调用insertCell()或dropCell()，
** 因此我们需要调用relinkCellList()来清理那些其他例程留下的混乱。
**
** pCur指向与调用此例程时相同的单元，即使该单元被移动到不同的页面。
** pCur可以为NULL。如果您不关心跟踪单元，请将pCur参数设置为NULL，
** 因为这将节省此例程跟踪它的工作。
**
** 请注意，当调用此例程时，pPage上的一些单元实际上可能未存储在pPage->u.aDisk[]中。
** 如果页面过满，可能会发生这种情况。此例程的部分工作是确保pPage的所有单元
** 再次适合pPage->u.aDisk[]。
**
** 在平衡pPage的兄弟页面的过程中，pPage的父页面可能会变得过满或未满。
** 如果发生这种情况，则在父页面上递归调用此例程。
**
** 如果此例程因任何原因失败，它可能会使数据库处于损坏状态。
** 因此，如果此例程失败，则应回滚数据库。
*/
static int balance(Btree *pBt, MemPage *pPage, BtCursor *pCur) {
  MemPage *pParent;        /* The parent of pPage */
                           /* pPage 的父页面 */
  MemPage *apOld[3];       /* pPage and up to two siblings */
                           /* pPage 和最多两个兄弟页面 */
  Pgno pgnoOld[3];         /* Page numbers for each page in apOld[] */
                           /* apOld[] 中每个页面的页码 */
  MemPage *apNew[4];       /* pPage and up to 3 siblings after balancing */
                           /* 平衡后的 pPage 和最多 3 个兄弟页面 */
  Pgno pgnoNew[4];         /* Page numbers for each page in apNew[] */
                           /* apNew[] 中每个页面的页码 */
  int idxDiv[3];           /* Indices of divider cells in pParent */
                           /* pParent 中分隔单元的索引 */
  Cell *apDiv[3];          /* Divider cells in pParent */
                           /* pParent 中的分隔单元 */
  int nCell;               /* Number of cells in apCell[] */
                           /* apCell[] 中的单元数 */
  int nOld;                /* Number of pages in apOld[] */
                           /* apOld[] 中的页面数 */
  int nNew;                /* Number of pages in apNew[] */
                           /* apNew[] 中的页面数 */
  int nDiv;                /* Number of cells in apDiv[] */
                           /* apDiv[] 中的单元数 */
  int i, j, k;             /* Loop counters */
                           /* 循环计数器 */
  int idx;                 /* Index of pPage in pParent->apCell[] */
                           /* pPage 在 pParent->apCell[] 中的索引 */
  int nxDiv;               /* Next divider slot in pParent->apCell[] */
                           /* pParent->apCell[] 中的下一个分隔槽 */
  int rc;                  /* The return code */
                           /* 返回代码 */
  int iCur;                /* apCell[iCur] is the cell of the cursor */
                           /* apCell[iCur] 是游标的单元 */
  MemPage *pOldCurPage;    /* The cursor originally points to this page */
                           /* 游标最初指向此页面 */
  int totalSize;           /* Total bytes for all cells */
                           /* 所有单元的总字节数 */
  int subtotal;            /* Subtotal of bytes in cells on one page */
                           /* 一个页面上单元字节的小计 */
  int cntNew[4];           /* Index in apCell[] of cell after i-th page */
                           /* 第 i 页之后单元在 apCell[] 中的索引 */
  int szNew[4];            /* Combined size of cells place on i-th page */
                           /* 放置在第 i 页上的单元的组合大小 */
  MemPage *extraUnref = 0; /* A page that needs to be unref-ed */
                           /* 需要取消引用的页面 */
  Pgno pgno;               /* Page number */
                           /* 页码 */
  Cell *apCell[MX_CELL * 3 + 5]; /* All cells from pages being balanceed */
                                 /* 来自正在平衡的页面的所有单元 */
  int szCell[MX_CELL * 3 + 5];   /* Local size of all cells */
                                 /* 所有单元的本地大小 */
  Cell aTemp[2];                 /* Temporary holding area for apDiv[] */
                                 /* apDiv[] 的临时保存区域 */
  MemPage aOld[3]; /* Temporary copies of pPage and its siblings */
                   /* pPage 及其兄弟页面的临时副本 */

  /*
  ** Return without doing any work if pPage is neither overfull nor
  ** underfull.
  **
  ** 如果pPage既不过满也不未满，则不执行任何工作直接返回。
  */
  assert(sqlitepager_iswriteable(pPage));
  if (!pPage->isOverfull && pPage->nFree < SQLITE_PAGE_SIZE / 2 &&
      pPage->nCell >= 2) {
    relinkCellList(pPage);
    return SQLITE_OK;
  }

  /*
  ** Find the parent of the page to be balanceed.
  ** If there is no parent, it means this page is the root page and
  ** special rules apply.
  **
  ** 查找要平衡的页面的父页面。
  ** 如果没有父页面，则意味着此页面是根页面，适用特殊规则。
  */
  pParent = pPage->pParent;
  if (pParent == 0) {
    Pgno pgnoChild;
    MemPage *pChild;
    if (pPage->nCell == 0) {
      if (pPage->u.hdr.rightChild) {
        /*
        ** The root page is empty.  Copy the one child page
        ** into the root page and return.  This reduces the depth
        ** of the BTree by one.
        **
        ** 根页面为空。将一个子页面复制到根页面并返回。
        ** 这将B树的深度减少一。
        */
        pgnoChild = pPage->u.hdr.rightChild;
        rc = sqlitepager_get(pBt->pPager, pgnoChild, (void **)&pChild);
        if (rc)
          return rc;
        memcpy(pPage, pChild, SQLITE_PAGE_SIZE);
        pPage->isInit = 0;
        rc = initPage(pPage, sqlitepager_pagenumber(pPage), 0);
        assert(rc == SQLITE_OK);
        reparentChildPages(pBt->pPager, pPage);
        if (pCur && pCur->pPage == pChild) {
          sqlitepager_unref(pChild);
          pCur->pPage = pPage;
          sqlitepager_ref(pPage);
        }
        freePage(pBt, pChild, pgnoChild);
        sqlitepager_unref(pChild);
      } else {
        relinkCellList(pPage);
      }
      return SQLITE_OK;
    }
    if (!pPage->isOverfull) {
      /* It is OK for the root page to be less than half full.
      **
      ** 根页面少于半满是可以的。
      */
      relinkCellList(pPage);
      return SQLITE_OK;
    }
    /*
    ** If we get to here, it means the root page is overfull.
    ** When this happens, Create a new child page and copy the
    ** contents of the root into the child.  Then make the root
    ** page an empty page with rightChild pointing to the new
    ** child.  Then fall thru to the code below which will cause
    ** the overfull child page to be split.
    **
    ** 如果我们到了这里，这意味着根页面过满。
    ** 当发生这种情况时，创建一个新的子页面并将根的内容复制到子页面中。
    ** 然后使根页面成为一个空页面，其rightChild指向新的子页面。
    ** 然后继续执行下面的代码，这将导致过满的子页面被拆分。
    */
    rc = sqlitepager_write(pPage);
    if (rc)
      return rc;
    rc = allocatePage(pBt, &pChild, &pgnoChild);
    if (rc)
      return rc;
    assert(sqlitepager_iswriteable(pChild));
    copyPage(pChild, pPage);
    pChild->pParent = pPage;
    sqlitepager_ref(pPage);
    pChild->isOverfull = 1;
    if (pCur && pCur->pPage == pPage) {
      sqlitepager_unref(pPage);
      pCur->pPage = pChild;
    } else {
      extraUnref = pChild;
    }
    zeroPage(pPage);
    pPage->u.hdr.rightChild = pgnoChild;
    pParent = pPage;
    pPage = pChild;
  }
  rc = sqlitepager_write(pParent);
  if (rc)
    return rc;

  /*
  ** Find the Cell in the parent page whose h.leftChild points back
  ** to pPage.  The "idx" variable is the index of that cell.  If pPage
  ** is the rightmost child of pParent then set idx to pParent->nCell
  **
  ** 在父页面中查找其h.leftChild指向pPage的单元。
  ** “idx”变量是该单元的索引。如果pPage是pParent的最右侧子页面，
  ** 则将idx设置为pParent->nCell
  */
  idx = -1;
  pgno = sqlitepager_pagenumber(pPage);
  for (i = 0; i < pParent->nCell; i++) {
    if (pParent->apCell[i]->h.leftChild == pgno) {
      idx = i;
      break;
    }
  }
  if (idx < 0 && pParent->u.hdr.rightChild == pgno) {
    idx = pParent->nCell;
  }
  if (idx < 0) {
    return SQLITE_CORRUPT;
  }

  /*
  ** Initialize variables so that it will be safe to jump
  ** directly to balance_cleanup at any moment.
  **
  ** 初始化变量，以便随时跳转到balance_cleanup是安全的。
  */
  nOld = nNew = 0;
  sqlitepager_ref(pParent);

  /*
  ** Find sibling pages to pPage and the Cells in pParent that divide
  ** the siblings.  An attempt is made to find one sibling on either
  ** side of pPage.  Both siblings are taken from one side, however, if
  ** pPage is either the first or last child of its parent.  If pParent
  ** has 3 or fewer children then all children of pParent are taken.
  **
  ** 查找pPage的兄弟页面以及pParent中分隔兄弟页面的单元。
  ** 尝试在pPage的两侧各找到一个兄弟页面。但是，如果pPage是其父页面的
  ** 第一个或最后一个子页面，则两个兄弟页面都取自一侧。
  ** 如果pParent有3个或更少的子页面，则获取pParent的所有子页面。
  */
  if (idx == pParent->nCell) {
    nxDiv = idx - 2;
  } else {
    nxDiv = idx - 1;
  }
  if (nxDiv < 0)
    nxDiv = 0;
  nDiv = 0;
  for (i = 0, k = nxDiv; i < 3; i++, k++) {
    if (k < pParent->nCell) {
      idxDiv[i] = k;
      apDiv[i] = pParent->apCell[k];
      nDiv++;
      pgnoOld[i] = apDiv[i]->h.leftChild;
    } else if (k == pParent->nCell) {
      pgnoOld[i] = pParent->u.hdr.rightChild;
    } else {
      break;
    }
    rc = sqlitepager_get(pBt->pPager, pgnoOld[i], (void **)&apOld[i]);
    if (rc)
      goto balance_cleanup;
    rc = initPage(apOld[i], pgnoOld[i], pParent);
    if (rc)
      goto balance_cleanup;
    nOld++;
  }

  /*
  ** Set iCur to be the index in apCell[] of the cell that the cursor
  ** is pointing to.  We will need this later on in order to keep the
  ** cursor pointing at the same cell.  If pCur points to a page that
  ** has no involvement with this rebalancing, then set iCur to a large
  ** number so that the iCur==j tests always fail in the main cell
  ** distribution loop below.
  **
  ** 将iCur设置为游标指向的单元在apCell[]中的索引。
  ** 我们稍后将需要它，以便保持游标指向同一个单元。
  ** 如果pCur指向与此重新平衡无关的页面，则将iCur设置为一个大数字，
  ** 以便iCur==j测试在下面的主单元分配循环中始终失败。
  */
  if (pCur) {
    iCur = 0;
    for (i = 0; i < nOld; i++) {
      if (pCur->pPage == apOld[i]) {
        iCur += pCur->idx;
        break;
      }
      iCur += apOld[i]->nCell;
      if (i < nOld - 1 && pCur->pPage == pParent && pCur->idx == idxDiv[i]) {
        break;
      }
      iCur++;
    }
    pOldCurPage = pCur->pPage;
  }

  /*
  ** Make copies of the content of pPage and its siblings into aOld[].
  ** The rest of this function will use data from the copies rather
  ** that the original pages since the original pages will be in the
  ** process of being overwritten.
  **
  ** 将pPage及其兄弟页面的内容复制到aOld[]中。
  ** 此函数的其余部分将使用副本中的数据，而不是原始页面，
  ** 因为原始页面将被覆盖。
  */
  for (i = 0; i < nOld; i++) {
    copyPage(&aOld[i], apOld[i]);
    rc = freePage(pBt, apOld[i], pgnoOld[i]);
    if (rc)
      goto balance_cleanup;
    sqlitepager_unref(apOld[i]);
    apOld[i] = &aOld[i];
  }

  /*
  ** Load pointers to all cells on sibling pages and the divider cells
  ** into the local apCell[] array.  Make copies of the divider cells
  ** into aTemp[] and remove the the divider Cells from pParent.
  **
  ** 将兄弟页面上的所有单元和分隔单元的指针加载到本地apCell[]数组中。
  ** 将分隔单元复制到aTemp[]中，并从pParent中删除分隔单元。
  */
  nCell = 0;
  for (i = 0; i < nOld; i++) {
    MemPage *pOld = apOld[i];
    for (j = 0; j < pOld->nCell; j++) {
      apCell[nCell] = pOld->apCell[j];
      szCell[nCell] = cellSize(apCell[nCell]);
      nCell++;
    }
    if (i < nOld - 1) {
      szCell[nCell] = cellSize(apDiv[i]);
      memcpy(&aTemp[i], apDiv[i], szCell[nCell]);
      apCell[nCell] = &aTemp[i];
      dropCell(pParent, nxDiv, szCell[nCell]);
      assert(apCell[nCell]->h.leftChild == pgnoOld[i]);
      apCell[nCell]->h.leftChild = pOld->u.hdr.rightChild;
      nCell++;
    }
  }

  /*
  ** Figure out the number of pages needed to hold all nCell cells.
  ** Store this number in "k".  Also compute szNew[] which is the total
  ** size of all cells on the i-th page and cntNew[] which is the index
  ** in apCell[] of the cell that divides path i from path i+1.
  ** cntNew[k] should equal nCell.
  **
  ** This little patch of code is critical for keeping the tree
  ** balanced.
  **
  ** 计算容纳所有nCell个单元所需的页面数。将此数字存储在“k”中。
  ** 还要计算szNew[]（第i页上所有单元的总大小）和cntNew[]
  ** （apCell[]中分隔路径i和路径i+1的单元的索引）。
  ** cntNew[k]应等于nCell。
  **
  ** 这小段代码对于保持树的平衡至关重要。
  */
  totalSize = 0;
  for (i = 0; i < nCell; i++) {
    totalSize += szCell[i];
  }
  for (subtotal = k = i = 0; i < nCell; i++) {
    subtotal += szCell[i];
    if (subtotal > USABLE_SPACE) {
      szNew[k] = subtotal - szCell[i];
      cntNew[k] = i;
      subtotal = 0;
      k++;
    }
  }
  szNew[k] = subtotal;
  cntNew[k] = nCell;
  k++;
  for (i = k - 1; i > 0; i--) {
    while (szNew[i] < USABLE_SPACE / 2) {
      cntNew[i - 1]--;
      assert(cntNew[i - 1] > 0);
      szNew[i] += szCell[cntNew[i - 1]];
      szNew[i - 1] -= szCell[cntNew[i - 1] - 1];
    }
  }
  assert(cntNew[0] > 0);

  /*
  ** Allocate k new pages
  **
  ** 分配k个新页面
  */
  for (i = 0; i < k; i++) {
    rc = allocatePage(pBt, &apNew[i], &pgnoNew[i]);
    if (rc)
      goto balance_cleanup;
    nNew++;
    zeroPage(apNew[i]);
    apNew[i]->isInit = 1;
  }

  /*
  ** Put the new pages in accending order.  This helps to
  ** keep entries in the disk file in order so that a scan
  ** of the table is a linear scan through the file.  That
  ** in turn helps the operating system to deliver pages
  ** from the disk more rapidly.
  **
  ** An O(n^2) insertion sort algorithm is used, but since
  ** n is never more than 3, that should not be a problem.
  **
  ** This one optimization makes the database about 25%
  ** faster for large insertions and deletions.
  **
  ** 将新页面按升序排列。这有助于使磁盘文件中的条目保持有序，
  ** 从而使表扫描成为文件的线性扫描。这反过来又有助于操作系统
  ** 更快地从磁盘传送页面。
  **
  ** 使用了O(n^2)插入排序算法，但由于n永远不会超过3，
  ** 所以这应该不是问题。
  **
  ** 这一优化使数据库的大型插入和删除操作速度提高了约25%。
  */
  for (i = 0; i < k - 1; i++) {
    int minV = pgnoNew[i];
    int minI = i;
    for (j = i + 1; j < k; j++) {
      if (pgnoNew[j] < minV) {
        minI = j;
        minV = pgnoNew[j];
      }
    }
    if (minI > i) {
      int t;
      MemPage *pT;
      t = pgnoNew[i];
      pT = apNew[i];
      pgnoNew[i] = pgnoNew[minI];
      apNew[i] = apNew[minI];
      pgnoNew[minI] = t;
      apNew[minI] = pT;
    }
  }

  /*
  ** Evenly distribute the data in apCell[] across the new pages.
  ** Insert divider cells into pParent as necessary.
  **
  ** 将apCell[]中的数据均匀分布到新页面中。
  ** 根据需要将分隔单元插入pParent。
  */
  j = 0;
  for (i = 0; i < nNew; i++) {
    MemPage *pNew = apNew[i];
    while (j < cntNew[i]) {
      assert(pNew->nFree >= szCell[j]);
      if (pCur && iCur == j) {
        pCur->pPage = pNew;
        pCur->idx = pNew->nCell;
      }
      insertCell(pNew, pNew->nCell, apCell[j], szCell[j]);
      j++;
    }
    assert(pNew->nCell > 0);
    assert(!pNew->isOverfull);
    relinkCellList(pNew);
    if (i < nNew - 1 && j < nCell) {
      pNew->u.hdr.rightChild = apCell[j]->h.leftChild;
      apCell[j]->h.leftChild = pgnoNew[i];
      if (pCur && iCur == j) {
        pCur->pPage = pParent;
        pCur->idx = nxDiv;
      }
      insertCell(pParent, nxDiv, apCell[j], szCell[j]);
      j++;
      nxDiv++;
    }
  }
  assert(j == nCell);
  apNew[nNew - 1]->u.hdr.rightChild = apOld[nOld - 1]->u.hdr.rightChild;
  if (nxDiv == pParent->nCell) {
    pParent->u.hdr.rightChild = pgnoNew[nNew - 1];
  } else {
    pParent->apCell[nxDiv]->h.leftChild = pgnoNew[nNew - 1];
  }
  if (pCur) {
    if (j <= iCur && pCur->pPage == pParent && pCur->idx > idxDiv[nOld - 1]) {
      assert(pCur->pPage == pOldCurPage);
      pCur->idx += nNew - nOld;
    } else {
      assert(pOldCurPage != 0);
      sqlitepager_ref(pCur->pPage);
      sqlitepager_unref(pOldCurPage);
    }
  }

  /*
  ** Reparent children of all cells.
  **
  ** 重置所有单元的子页面的父级。
  */
  for (i = 0; i < nNew; i++) {
    reparentChildPages(pBt->pPager, apNew[i]);
  }
  reparentChildPages(pBt->pPager, pParent);

  /*
  ** balance the parent page.
  **
  ** 平衡父页面。
  */
  rc = balance(pBt, pParent, pCur);

  /*
  ** Cleanup before returning.
  **
  ** 返回前清理。
  */
balance_cleanup:
  if (extraUnref) {
    sqlitepager_unref(extraUnref);
  }
  for (i = 0; i < nOld; i++) {
    if (apOld[i] != &aOld[i])
      sqlitepager_unref(apOld[i]);
  }
  for (i = 0; i < nNew; i++) {
    sqlitepager_unref(apNew[i]);
  }
  if (pCur && pCur->pPage == 0) {
    pCur->pPage = pParent;
    pCur->idx = 0;
  } else {
    sqlitepager_unref(pParent);
  }
  return rc;
}

/*
 ** Insert a new record into the BTree.  The key is given by (pKey,nKey)
 ** and the data is given by (pData,nData).  The cursor is used only to
 ** define what database the record should be inserted into.  The cursor
 ** is left pointing at the new record.
 **
 ** 将新记录插入B树。键由(pKey,nKey)给出，数据由(pData,nData)给出。
 ** 游标仅用于定义记录应插入哪个数据库。游标指向新记录。
 */
int sqliteBtreeInsert(
    BtCursor *pCur,              /* Insert data into the table of this cursor */
                                 /* 将数据插入此游标的表中 */
    const void *pKey, int nKey,  /* The key of the new record */
                                 /* 新记录的键 */
    const void *pData, int nData /* The data of the new record */
                                 /* 新记录的数据 */
) {
  Cell newCell;
  int rc;
  int loc;
  int szNew;
  MemPage *pPage;
  Btree *pBt = pCur->pBt;

  if (pCur->pPage == 0) {
    return SQLITE_ABORT; /* A rollback destroyed this cursor */
  }
  if (!pCur->pBt->inTrans || nKey + nData == 0) {
    return SQLITE_ERROR; /* Must start a transaction first */
  }
  if (!pCur->wrFlag) {
    return SQLITE_PERM; /* Cursor not open for writing */
  }
  rc = sqliteBtreeMoveto(pCur, pKey, nKey, &loc);
  if (rc)
    return rc;
  pPage = pCur->pPage;
  rc = sqlitepager_write(pPage);
  if (rc)
    return rc;
  rc = fillInCell(pBt, &newCell, pKey, nKey, pData, nData);
  if (rc)
    return rc;
  szNew = cellSize(&newCell);
  if (loc == 0) {
    newCell.h.leftChild = pPage->apCell[pCur->idx]->h.leftChild;
    rc = clearCell(pBt, pPage->apCell[pCur->idx]);
    if (rc)
      return rc;
    dropCell(pPage, pCur->idx, cellSize(pPage->apCell[pCur->idx]));
  } else if (loc < 0 && pPage->nCell > 0) {
    assert(pPage->u.hdr.rightChild == 0); /* Must be a leaf page */
    pCur->idx++;
  } else {
    assert(pPage->u.hdr.rightChild == 0); /* Must be a leaf page */
  }
  insertCell(pPage, pCur->idx, &newCell, szNew);
  rc = balance(pCur->pBt, pPage, pCur);
  /* sqliteBtreePageDump(pCur->pBt, pCur->pgnoRoot, 1); */
  /* fflush(stdout); */
  return rc;
}

/*
** Delete the entry that the cursor is pointing to.
**
** The cursor is left pointing at either the next or the previous
** entry.  If the cursor is left pointing to the next entry, then
** the pCur->bSkipNext flag is set which forces the next call to
** sqliteBtreeNext() to be a no-op.  That way, you can always call
** sqliteBtreeNext() after a delete and the cursor will be left
** pointing to the first entry after the deleted entry.
**
** 删除游标指向的条目。
**
** 游标指向下一个或上一个条目。如果游标指向下一个条目，
** 则设置pCur->bSkipNext标志，这将强制下一次调用sqliteBtreeNext()为无操作。
** 这样，您总是可以在删除后调用sqliteBtreeNext()，游标将指向已删除条目之后的第一个条目。
*/
int sqliteBtreeDelete(BtCursor *pCur) {
  MemPage *pPage = pCur->pPage;
  Cell *pCell;
  int rc;
  Pgno pgnoChild;

  if (pCur->pPage == 0) {
    return SQLITE_ABORT; /* A rollback destroyed this cursor */
  }
  if (!pCur->pBt->inTrans) {
    return SQLITE_ERROR; /* Must start a transaction first */
  }
  if (pCur->idx >= pPage->nCell) {
    return SQLITE_ERROR; /* The cursor is not pointing to anything */
  }
  if (!pCur->wrFlag) {
    return SQLITE_PERM; /* Did not open this cursor for writing */
  }
  rc = sqlitepager_write(pPage);
  if (rc)
    return rc;
  pCell = pPage->apCell[pCur->idx];
  pgnoChild = pCell->h.leftChild;
  clearCell(pCur->pBt, pCell);
  if (pgnoChild) {
    /*
    ** The entry we are about to delete is not a leaf so if we do not
    ** do something we will leave a hole on an internal page.
    ** We have to fill the hole by moving in a cell from a leaf.  The
    ** next Cell after the one to be deleted is guaranteed to exist and
    ** to be a leaf so we can use it.
    **
    ** 我们要删除的条目不是叶子，所以如果我们不采取措施，我们将在内部页面上留下一个洞。
    ** 我们必须通过从叶子移入一个单元来填补这个洞。
    ** 待删除单元之后的下一个单元保证存在并且是叶子，因此我们可以使用它。
    */
    BtCursor leafCur;
    Cell *pNext;
    int szNext;
    getTempCursor(pCur, &leafCur);
    rc = sqliteBtreeNext(&leafCur, 0);
    if (rc != SQLITE_OK) {
      return SQLITE_CORRUPT;
    }
    rc = sqlitepager_write(leafCur.pPage);
    if (rc)
      return rc;
    dropCell(pPage, pCur->idx, cellSize(pCell));
    pNext = leafCur.pPage->apCell[leafCur.idx];
    szNext = cellSize(pNext);
    pNext->h.leftChild = pgnoChild;
    insertCell(pPage, pCur->idx, pNext, szNext);
    rc = balance(pCur->pBt, pPage, pCur);
    if (rc)
      return rc;
    pCur->bSkipNext = 1;
    dropCell(leafCur.pPage, leafCur.idx, szNext);
    rc = balance(pCur->pBt, leafCur.pPage, pCur);
    releaseTempCursor(&leafCur);
  } else {
    dropCell(pPage, pCur->idx, cellSize(pCell));
    if (pCur->idx >= pPage->nCell) {
      pCur->idx = pPage->nCell - 1;
      if (pCur->idx < 0) {
        pCur->idx = 0;
        pCur->bSkipNext = 1;
      } else {
        pCur->bSkipNext = 0;
      }
    } else {
      pCur->bSkipNext = 1;
    }
    rc = balance(pCur->pBt, pPage, pCur);
  }
  return rc;
}

/*
** Create a new BTree table.  Write into *piTable the page
** number for the root page of the new table.
**
** In the current implementation, BTree tables and BTree indices are the
** the same.  But in the future, we may change this so that BTree tables
** are restricted to having a 4-byte integer key and arbitrary data and
** BTree indices are restricted to having an arbitrary key and no data.
**
** 创建一个新的B树表。将新表的根页面的页码写入*piTable。
**
** 在当前的实现中，B树表和B树索引是相同的。但在未来，我们可能会更改此设置，
** 以便B树表仅限于具有4字节整数键和任意数据，而B树索引仅限于具有任意键和无数据。
*/
int sqliteBtreeCreateTable(Btree *pBt, int *piTable) {
  MemPage *pRoot;
  Pgno pgnoRoot;
  int rc;
  if (!pBt->inTrans) {
    return SQLITE_ERROR; /* Must start a transaction first */
  }
  if (pBt->readOnly) {
    return SQLITE_READONLY;
  }
  rc = allocatePage(pBt, &pRoot, &pgnoRoot);
  if (rc)
    return rc;
  assert(sqlitepager_iswriteable(pRoot));
  zeroPage(pRoot);
  sqlitepager_unref(pRoot);
  *piTable = (int)pgnoRoot;
  return SQLITE_OK;
}

/*
** Create a new BTree index.  Write into *piTable the page
** number for the root page of the new index.
**
** In the current implementation, BTree tables and BTree indices are the
** the same.  But in the future, we may change this so that BTree tables
** are restricted to having a 4-byte integer key and arbitrary data and
** BTree indices are restricted to having an arbitrary key and no data.
**
** 创建一个新的B树索引。将新索引的根页面的页码写入*piTable。
**
** 在当前的实现中，B树表和B树索引是相同的。但在未来，我们可能会更改此设置，
** 以便B树表仅限于具有4字节整数键和任意数据，而B树索引仅限于具有任意键和无数据。
*/
int sqliteBtreeCreateIndex(Btree *pBt, int *piIndex) {
  return sqliteBtreeCreateTable(pBt, piIndex);
}

/*
** Erase the given database page and all its children.  Return
** the page to the freelist.
**
** 擦除给定的数据库页面及其所有子页面。将页面返回到空闲列表。
*/
static int clearDatabasePage(Btree *pBt, Pgno pgno, int freePageFlag) {
  MemPage *pPage;
  int rc;
  Cell *pCell;
  int idx;

  rc = sqlitepager_get(pBt->pPager, pgno, (void **)&pPage);
  if (rc)
    return rc;
  rc = sqlitepager_write(pPage);
  if (rc)
    return rc;
  idx = pPage->u.hdr.firstCell;
  while (idx > 0) {
    pCell = (Cell *)&pPage->u.aDisk[idx];
    idx = pCell->h.iNext;
    if (pCell->h.leftChild) {
      rc = clearDatabasePage(pBt, pCell->h.leftChild, 1);
      if (rc)
        return rc;
    }
    rc = clearCell(pBt, pCell);
    if (rc)
      return rc;
  }
  if (pPage->u.hdr.rightChild) {
    rc = clearDatabasePage(pBt, pPage->u.hdr.rightChild, 1);
    if (rc)
      return rc;
  }
  if (freePageFlag) {
    rc = freePage(pBt, pPage, pgno);
  } else {
    zeroPage(pPage);
  }
  sqlitepager_unref(pPage);
  return rc;
}

/*
** Delete all information from a single table in the database.
**
** 删除数据库中单个表的所有信息。
*/
int sqliteBtreeClearTable(Btree *pBt, int iTable) {
  int rc;
  ptr nLock;
  if (!pBt->inTrans) {
    return SQLITE_ERROR; /* Must start a transaction first */
  }
  if (pBt->readOnly) {
    return SQLITE_READONLY;
  }
  nLock = (ptr)sqliteHashFind(&pBt->locks, 0, iTable);
  if (nLock) {
    return SQLITE_LOCKED;
  }
  rc = clearDatabasePage(pBt, (Pgno)iTable, 0);
  if (rc) {
    sqliteBtreeRollback(pBt);
  }
  return rc;
}

/*
** Erase all information in a table and add the root of the table to
** the freelist.  Except, the root of the principle table (the one on
** page 2) is never added to the freelist.
**
** 擦除表中的所有信息，并将表的根添加到空闲列表。
** 但是，主表的根（第2页上的那个）永远不会添加到空闲列表。
*/
int sqliteBtreeDropTable(Btree *pBt, int iTable) {
  int rc;
  MemPage *pPage;
  if (!pBt->inTrans) {
    return SQLITE_ERROR; /* Must start a transaction first */
  }
  if (pBt->readOnly) {
    return SQLITE_READONLY;
  }
  rc = sqlitepager_get(pBt->pPager, (Pgno)iTable, (void **)&pPage);
  if (rc)
    return rc;
  rc = sqliteBtreeClearTable(pBt, iTable);
  if (rc)
    return rc;
  if (iTable > 2) {
    rc = freePage(pBt, pPage, iTable);
  } else {
    zeroPage(pPage);
  }
  sqlitepager_unref(pPage);
  return rc;
}

/*
** Read the meta-information out of a database file.
**
** 从数据库文件中读取元信息。
*/
int sqliteBtreeGetMeta(Btree *pBt, int *aMeta) {
  PageOne *pP1;
  int rc;

  rc = sqlitepager_get(pBt->pPager, 1, (void **)&pP1);
  if (rc)
    return rc;
  aMeta[0] = pP1->nFree;
  memcpy(&aMeta[1], pP1->aMeta, sizeof(pP1->aMeta));
  sqlitepager_unref(pP1);
  return SQLITE_OK;
}

/*
** Write meta-information back into the database.
**
** 将元信息写回数据库。
*/
int sqliteBtreeUpdateMeta(Btree *pBt, int *aMeta) {
  PageOne *pP1;
  int rc;
  if (!pBt->inTrans) {
    return SQLITE_ERROR; /* Must start a transaction first */
  }
  if (pBt->readOnly) {
    return SQLITE_READONLY;
  }
  pP1 = pBt->page1;
  rc = sqlitepager_write(pP1);
  if (rc)
    return rc;
  memcpy(pP1->aMeta, &aMeta[1], sizeof(pP1->aMeta));
  return SQLITE_OK;
}

/******************************************************************************
** The complete implementation of the BTree subsystem is above this line.
** All the code the follows is for testing and troubleshooting the BTree
** subsystem.  None of the code that follows is used during normal operation.
**
** BTree子系统的完整实现位于此行之上。
** 以下所有代码均用于BTree子系统的测试和故障排除。
** 以下代码均不用于正常操作。
******************************************************************************/

/*
** Print a disassembly of the given page on standard output.  This routine
** is used for debugging and testing only.
**
** 在标准输出上打印给定页面的反汇编。此例程仅用于调试和测试。
*/
#ifdef SQLITE_TEST
int sqliteBtreePageDump(Btree *pBt, int pgno, int recursive) {
  int rc;
  MemPage *pPage;
  int i, j;
  int nFree;
  u16 idx;
  char range[20];
  unsigned char payload[20];
  rc = sqlitepager_get(pBt->pPager, (Pgno)pgno, (void **)&pPage);
  if (rc) {
    return rc;
  }
  if (recursive)
    printf("PAGE %d:\n", pgno);
  i = 0;
  idx = pPage->u.hdr.firstCell;
  while (idx > 0 && idx <= SQLITE_PAGE_SIZE - MIN_CELL_SIZE) {
    Cell *pCell = (Cell *)&pPage->u.aDisk[idx];
    int sz = cellSize(pCell);
    sprintf(range, "%d..%d", idx, idx + sz - 1);
    sz = NKEY(pCell->h) + NDATA(pCell->h);
    if (sz > sizeof(payload) - 1)
      sz = sizeof(payload) - 1;
    memcpy(payload, pCell->aPayload, sz);
    for (j = 0; j < sz; j++) {
      if (payload[j] < 0x20 || payload[j] > 0x7f)
        payload[j] = '.';
    }
    payload[sz] = 0;
    printf("cell %2d: i=%-10s chld=%-4d nk=%-4d nd=%-4d payload=%s\n", i, range,
           (int)pCell->h.leftChild, NKEY(pCell->h), NDATA(pCell->h), payload);
    if (pPage->isInit && pPage->apCell[i] != pCell) {
      printf("**** apCell[%d] does not match on prior entry ****\n", i);
    }
    i++;
    idx = pCell->h.iNext;
  }
  if (idx != 0) {
    printf("ERROR: next cell index out of range: %d\n", idx);
  }
  printf("right_child: %d\n", pPage->u.hdr.rightChild);
  nFree = 0;
  i = 0;
  idx = pPage->u.hdr.firstFree;
  while (idx > 0 && idx < SQLITE_PAGE_SIZE) {
    FreeBlk *p = (FreeBlk *)&pPage->u.aDisk[idx];
    sprintf(range, "%d..%d", idx, idx + p->iSize - 1);
    nFree += p->iSize;
    printf("freeblock %2d: i=%-10s size=%-4d total=%d\n", i, range, p->iSize,
           nFree);
    idx = p->iNext;
    i++;
  }
  if (idx != 0) {
    printf("ERROR: next freeblock index out of range: %d\n", idx);
  }
  if (recursive && pPage->u.hdr.rightChild != 0) {
    idx = pPage->u.hdr.firstCell;
    while (idx > 0 && idx < SQLITE_PAGE_SIZE - MIN_CELL_SIZE) {
      Cell *pCell = (Cell *)&pPage->u.aDisk[idx];
      sqliteBtreePageDump(pBt, pCell->h.leftChild, 1);
      idx = pCell->h.iNext;
    }
    sqliteBtreePageDump(pBt, pPage->u.hdr.rightChild, 1);
  }
  sqlitepager_unref(pPage);
  return SQLITE_OK;
}
#endif

#ifdef SQLITE_TEST
/*
** Fill aResult[] with information about the entry and page that the
** cursor is pointing to.
**
**   aResult[0] =  The page number
**   aResult[1] =  The entry number
**   aResult[2] =  Total number of entries on this page
**   aResult[3] =  Size of this entry
**   aResult[4] =  Number of free bytes on this page
**   aResult[5] =  Number of free blocks on the page
**   aResult[6] =  Page number of the left child of this entry
**   aResult[7] =  Page number of the right child for the whole page
**
** This routine is used for testing and debugging only.
**
** 用游标指向的条目和页面的信息填充aResult[]。
**
**   aResult[0] =  页码
**   aResult[1] =  条目编号
**   aResult[2] =  此页面上的条目总数
**   aResult[3] =  此条目的大小
**   aResult[4] =  此页面上的可用字节数
**   aResult[5] =  页面上的可用块数
**   aResult[6] =  此条目的左子页面的页码
**   aResult[7] =  整个页面的右子页面的页码
**
** 此例程仅用于测试和调试。
*/
int sqliteBtreeCursorDump(BtCursor *pCur, int *aResult) {
  int cnt, idx;
  MemPage *pPage = pCur->pPage;
  aResult[0] = sqlitepager_pagenumber(pPage);
  aResult[1] = pCur->idx;
  aResult[2] = pPage->nCell;
  if (pCur->idx >= 0 && pCur->idx < pPage->nCell) {
    aResult[3] = cellSize(pPage->apCell[pCur->idx]);
    aResult[6] = pPage->apCell[pCur->idx]->h.leftChild;
  } else {
    aResult[3] = 0;
    aResult[6] = 0;
  }
  aResult[4] = pPage->nFree;
  cnt = 0;
  idx = pPage->u.hdr.firstFree;
  while (idx > 0 && idx < SQLITE_PAGE_SIZE) {
    cnt++;
    idx = ((FreeBlk *)&pPage->u.aDisk[idx])->iNext;
  }
  aResult[5] = cnt;
  aResult[7] = pPage->u.hdr.rightChild;
  return SQLITE_OK;
}
#endif

#ifdef SQLITE_TEST
/*
** Return the pager associated with a BTree.  This routine is used for
** testing and debugging only.
**
** 返回与BTree关联的分页器。此例程仅用于测试和调试。
*/
Pager *sqliteBtreePager(Btree *pBt) { return pBt->pPager; }
#endif

/*
** This structure is passed around through all the sanity checking routines
** in order to keep track of some global state information.
**
** 此结构在所有健全性检查例程中传递，以便跟踪一些全局状态信息。
*/
typedef struct IntegrityCk IntegrityCk;
struct IntegrityCk {
  Btree *pBt;    /* The tree being checked out */
                 /* 正在检查的树 */
  Pager *pPager; /* The associated pager.  Also accessible by pBt->pPager */
                 /* 关联的分页器。也可以通过 pBt->pPager 访问 */
  int nPage;     /* Number of pages in the database */
                 /* 数据库中的页数 */
  int *anRef;    /* Number of times each page is referenced */
                 /* 每个页面被引用的次数 */
  int nTreePage; /* Number of BTree pages */
                 /* BTree 页数 */
  int nByte;     /* Number of bytes of data stored on BTree pages */
                 /* 存储在 BTree 页面上的数据字节数 */
  char *zErrMsg; /* An error message.  NULL of no errors seen. */
                 /* 错误消息。如果没有看到错误，则为 NULL。 */
};

/*
** Append a message to the error message string.
**
** 将消息附加到错误消息字符串。
*/
static void checkAppendMsg(IntegrityCk *pCheck, char *zMsg1, char *zMsg2) {
  if (pCheck->zErrMsg) {
    char *zOld = pCheck->zErrMsg;
    pCheck->zErrMsg = 0;
    sqliteSetString(&pCheck->zErrMsg, zOld, "\n", zMsg1, zMsg2, 0);
    sqliteFree(zOld);
  } else {
    sqliteSetString(&pCheck->zErrMsg, zMsg1, zMsg2, 0);
  }
}

/*
** Add 1 to the reference count for page iPage.  If this is the second
** reference to the page, add an error message to pCheck->zErrMsg.
** Return 1 if there are 2 ore more references to the page and 0 if
** if this is the first reference to the page.
**
** Also check that the page number is in bounds.
**
** 将页面iPage的引用计数加1。如果是对该页面的第二次引用，
** 则向pCheck->zErrMsg添加一条错误消息。
** 如果对该页面有2个或更多引用，则返回1；如果是对该页面的第一次引用，则返回0。
**
** 还要检查页码是否在界限内。
*/
static int checkRef(IntegrityCk *pCheck, int iPage, char *zContext) {
  if (iPage == 0)
    return 1;
  if (iPage > pCheck->nPage) {
    char zBuf[100];
    sprintf(zBuf, "invalid page number %d", iPage);
    checkAppendMsg(pCheck, zContext, zBuf);
    return 1;
  }
  if (pCheck->anRef[iPage] == 1) {
    char zBuf[100];
    sprintf(zBuf, "2nd reference to page %d", iPage);
    checkAppendMsg(pCheck, zContext, zBuf);
    return 1;
  }
  return (pCheck->anRef[iPage]++) > 1;
}

/*
** Check the integrity of the freelist or of an overflow page list.
** Verify that the number of pages on the list is N.
**
** 检查空闲列表或溢出页面列表的完整性。
** 验证列表上的页面数是否为N。
*/
static void checkList(
    IntegrityCk *pCheck, /* Integrity checking context */
                         /* 完整性检查上下文 */
    int isFreeList, /* True for a freelist.  False for overflow page list */
                    /* 对于空闲列表为 True。对于溢出页面列表为 False */
    int iPage,      /* Page number for first page in the list */
                    /* 列表中第一页的页码 */
    int N,          /* Expected number of pages in the list */
                    /* 列表中的预期页数 */
    char *zContext  /* Context for error messages */
                    /* 错误消息的上下文 */
) {
  int i;
  char zMsg[100];
  while (N-- > 0) {
    OverflowPage *pOvfl;
    if (iPage < 1) {
      sprintf(zMsg, "%d pages missing from overflow list", N + 1);
      checkAppendMsg(pCheck, zContext, zMsg);
      break;
    }
    if (checkRef(pCheck, iPage, zContext))
      break;
    if (sqlitepager_get(pCheck->pPager, (Pgno)iPage, (void **)&pOvfl)) {
      sprintf(zMsg, "failed to get page %d", iPage);
      checkAppendMsg(pCheck, zContext, zMsg);
      break;
    }
    if (isFreeList) {
      FreelistInfo *pInfo = (FreelistInfo *)pOvfl->aPayload;
      for (i = 0; i < pInfo->nFree; i++) {
        checkRef(pCheck, pInfo->aFree[i], zMsg);
      }
      N -= pInfo->nFree;
    }
    iPage = (int)pOvfl->iNext;
    sqlitepager_unref(pOvfl);
  }
}

/*
** Return negative if zKey1<zKey2.
** Return zero if zKey1==zKey2.
** Return positive if zKey1>zKey2.
**
** 如果zKey1<zKey2，则返回负数。
** 如果zKey1==zKey2，则返回零。
** 如果zKey1>zKey2，则返回正数。
*/
static int keyCompare(const char *zKey1, int nKey1, const char *zKey2,
                      int nKey2) {
  int min = nKey1 > nKey2 ? nKey2 : nKey1;
  int c = memcmp(zKey1, zKey2, min);
  if (c == 0) {
    c = nKey1 - nKey2;
  }
  return c;
}

/*
** Do various sanity checks on a single page of a tree.  Return
** the tree depth.  Root pages return 0.  Parents of root pages
** return 1, and so forth.
**
** These checks are done:
**
**      1.  Make sure that cells and freeblocks do not overlap
**          but combine to completely cover the page.
**      2.  Make sure cell keys are in order.
**      3.  Make sure no key is less than or equal to zLowerBound.
**      4.  Make sure no key is greater than or equal to zUpperBound.
**      5.  Check the integrity of overflow pages.
**      6.  Recursively call checkTreePage on all children.
**      7.  Verify that the depth of all children is the same.
**      8.  Make sure this page is at least 33% full or else it is
**          the root of the tree.
**
** 对树的单个页面进行各种健全性检查。返回树的深度。
** 根页面返回0。根页面的父页面返回1，依此类推。
**
** 执行以下检查：
**
**      1.  确保单元格和空闲块不重叠，但结合起来完全覆盖页面。
**      2.  确保单元格键按顺序排列。
**      3.  确保没有键小于或等于zLowerBound。
**      4.  确保没有键大于或等于zUpperBound。
**      5.  检查溢出页面的完整性。
**      6.  对所有子项递归调用checkTreePage。
**      7.  验证所有子项的深度是否相同。
**      8.  确保此页面至少已满33%，否则它是树的根。
*/
static int checkTreePage(
    IntegrityCk *pCheck,  /* Context for the sanity check */
                          /* 健全性检查的上下文 */
    int iPage,            /* Page number of the page to check */
                          /* 要检查的页面的页码 */
    MemPage *pParent,     /* Parent page */
                          /* 父页面 */
    char *zParentContext, /* Parent context */
                          /* 父上下文 */
    char *zLowerBound, /* All keys should be greater than this, if not NULL */
                       /* 如果不为 NULL，则所有键都应大于此值 */
    int nLower,        /* Number of characters in zLowerBound */
                       /* zLowerBound 中的字符数 */
    char *zUpperBound, /* All keys should be less than this, if not NULL */
                       /* 如果不为 NULL，则所有键都应小于此值 */
    int nUpper         /* Number of characters in zUpperBound */
                       /* zUpperBound 中的字符数 */
) {
  MemPage *pPage;
  int i, rc, depth, d2, pgno;
  char *zKey1, *zKey2;
  int nKey1, nKey2;
  BtCursor cur;
  char zMsg[100];
  char zContext[100];
  char hit[SQLITE_PAGE_SIZE];

  /* Check that the page exists
  **
  ** 检查页面是否存在
  */
  if (iPage == 0)
    return 0;
  if (checkRef(pCheck, iPage, zParentContext))
    return 0;
  sprintf(zContext, "On tree page %d: ", iPage);
  if ((rc = sqlitepager_get(pCheck->pPager, (Pgno)iPage, (void **)&pPage)) !=
      0) {
    sprintf(zMsg, "unable to get the page. error code=%d", rc);
    checkAppendMsg(pCheck, zContext, zMsg);
    return 0;
  }
  if ((rc = initPage(pPage, (Pgno)iPage, pParent)) != 0) {
    sprintf(zMsg, "initPage() returns error code %d", rc);
    checkAppendMsg(pCheck, zContext, zMsg);
    sqlitepager_unref(pPage);
    return 0;
  }

  /* Check out all the cells.
  **
  ** 检查所有单元格。
  */
  depth = 0;
  if (zLowerBound) {
    zKey1 = malloc(nLower + 1);
    memset(zKey1, 0, nLower + 1);
    memcpy(zKey1, zLowerBound, nLower);
    zKey1[nLower] = 0;
  } else {
    zKey1 = 0;
  }
  nKey1 = nLower;
  cur.pPage = pPage;
  cur.pBt = pCheck->pBt;
  for (i = 0; i < pPage->nCell; i++) {
    Cell *pCell = pPage->apCell[i];
    int sz;

    /* Check payload overflow pages
    **
    ** 检查有效负载溢出页面
    */
    nKey2 = NKEY(pCell->h);
    sz = nKey2 + NDATA(pCell->h);
    sprintf(zContext, "On page %d cell %d: ", iPage, i);
    if (sz > MX_LOCAL_PAYLOAD) {
      int nPage = (sz - MX_LOCAL_PAYLOAD + OVERFLOW_SIZE - 1) / OVERFLOW_SIZE;
      checkList(pCheck, 0, pCell->ovfl, nPage, zContext);
    }

    /* Check that keys are in the right order
    **
    ** 检查键是否按正确的顺序排列
    */
    cur.idx = i;
    zKey2 = sqliteMalloc(nKey2 + 1);
    getPayload(&cur, 0, nKey2, zKey2);
    if (zKey1 && keyCompare(zKey1, nKey1, zKey2, nKey2) >= 0) {
      checkAppendMsg(pCheck, zContext, "Key is out of order");
    }

    /* Check sanity of left child page.
    **
    ** 检查左子页面的健全性。
    */
    pgno = (int)pCell->h.leftChild;
    d2 = checkTreePage(pCheck, pgno, pPage, zContext, zKey1, nKey1, zKey2,
                       nKey2);
    if (i > 0 && d2 != depth) {
      checkAppendMsg(pCheck, zContext, "Child page depth differs");
    }
    depth = d2;
    sqliteFree(zKey1);
    zKey1 = zKey2;
    nKey1 = nKey2;
  }
  pgno = pPage->u.hdr.rightChild;
  sprintf(zContext, "On page %d at right child: ", iPage);
  checkTreePage(pCheck, pgno, pPage, zContext, zKey1, nKey1, zUpperBound,
                nUpper);
  sqliteFree(zKey1);

  /* Check for complete coverage of the page
  **
  ** 检查页面的完全覆盖情况
  */
  memset(hit, 0, sizeof(hit));
  memset(hit, 1, sizeof(PageHdr));
  for (i = pPage->u.hdr.firstCell; i > 0 && i < SQLITE_PAGE_SIZE;) {
    Cell *pCell = (Cell *)&pPage->u.aDisk[i];
    int j;
    for (j = i + cellSize(pCell) - 1; j >= i; j--)
      hit[j]++;
    i = pCell->h.iNext;
  }
  for (i = pPage->u.hdr.firstFree; i > 0 && i < SQLITE_PAGE_SIZE;) {
    FreeBlk *pFBlk = (FreeBlk *)&pPage->u.aDisk[i];
    int j;
    for (j = i + pFBlk->iSize - 1; j >= i; j--)
      hit[j]++;
    i = pFBlk->iNext;
  }
  for (i = 0; i < SQLITE_PAGE_SIZE; i++) {
    if (hit[i] == 0) {
      sprintf(zMsg, "Unused space at byte %d of page %d", i, iPage);
      checkAppendMsg(pCheck, zMsg, 0);
      break;
    } else if (hit[i] > 1) {
      sprintf(zMsg, "Multiple uses for byte %d of page %d", i, iPage);
      checkAppendMsg(pCheck, zMsg, 0);
      break;
    }
  }

  /* Check that free space is kept to a minimum
  **
  ** 检查可用空间是否保持在最低限度
  */
#if 0
  if( pParent && pParent->nCell>2 && pPage->nFree>3*SQLITE_PAGE_SIZE/4 ){
    sprintf(zMsg, "free space (%d) greater than max (%d)", pPage->nFree,
       SQLITE_PAGE_SIZE/3);
    checkAppendMsg(pCheck, zContext, zMsg);
  }
#endif

  /* Update freespace totals.
  **
  ** 更新可用空间总数。
  */
  pCheck->nTreePage++;
  pCheck->nByte += USABLE_SPACE - pPage->nFree;

  sqlitepager_unref(pPage);
  return depth;
}

/*
** This routine does a complete check of the given BTree file.  aRoot[] is
** an array of pages numbers were each page number is the root page of
** a table.  nRoot is the number of entries in aRoot.
**
** If everything checks out, this routine returns NULL.  If something is
** amiss, an error message is written into memory obtained from malloc()
** and a pointer to that error message is returned.  The calling function
** is responsible for freeing the error message when it is done.
**
** 此例程对给定的BTree文件进行完整检查。aRoot[]是一个页码数组，
** 其中每个页码都是表的根页面。nRoot是aRoot中的条目数。
**
** 如果一切正常，此例程返回NULL。如果出现问题，
** 则将错误消息写入从malloc()获得的内存中，并返回指向该错误消息的指针。
** 调用函数负责在完成后释放错误消息。
*/
char *sqliteBtreeIntegrityCheck(Btree *pBt, int *aRoot, int nRoot) {
  int i;
  int nRef;
  IntegrityCk sCheck;

  nRef = *sqlitepager_stats(pBt->pPager);
  if (lockBtree(pBt) != SQLITE_OK) {
    return sqliteStrDup("Unable to acquire a read lock on the database");
  }
  sCheck.pBt = pBt;
  sCheck.pPager = pBt->pPager;
  sCheck.nPage = sqlitepager_pagecount(sCheck.pPager);
  sCheck.anRef = malloc((sCheck.nPage + 1) * sizeof(sCheck.anRef[0]));
  memset(sCheck.anRef, 0, (sCheck.nPage + 1) * sizeof(sCheck.anRef[0]));

  sCheck.anRef[1] = 1;
  for (i = 2; i <= sCheck.nPage; i++) {
    sCheck.anRef[i] = 0;
  }
  sCheck.zErrMsg = 0;

  /* Check the integrity of the freelist
  **
  ** 检查空闲列表的完整性
  */
  checkList(&sCheck, 1, pBt->page1->freeList, pBt->page1->nFree,
            "Main freelist: ");

  /* Check all the tables.
  **
  ** 检查所有表。
  */
  for (i = 0; i < nRoot; i++) {
    if (aRoot[i] == 0)
      continue;
    checkTreePage(&sCheck, aRoot[i], 0, "List of tree roots: ", 0, 0, 0, 0);
  }

  /* Make sure every page in the file is referenced
  **
  ** 确保文件中的每个页面都被引用
  */
  for (i = 1; i <= sCheck.nPage; i++) {
    if (sCheck.anRef[i] == 0) {
      char zBuf[100];
      sprintf(zBuf, "Page %d is never used", i);
      checkAppendMsg(&sCheck, zBuf, 0);
    }
  }

  /* Make sure this analysis did not leave any unref() pages
  **
  ** 确保此分析没有留下任何unref()页面
  */
  unlockBtreeIfUnused(pBt);
  if (nRef != *sqlitepager_stats(pBt->pPager)) {
    char zBuf[100];
    sprintf(zBuf,
            "Outstanding page count goes from %d to %d during this analysis",
            nRef, *sqlitepager_stats(pBt->pPager));
    checkAppendMsg(&sCheck, zBuf, 0);
  }

  /* Clean  up and report errors.
  **
  ** 清理并报告错误。
  */
  sqliteFree(sCheck.anRef);
  return sCheck.zErrMsg;
}
