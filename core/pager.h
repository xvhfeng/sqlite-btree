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
** This header file defines the interface that the sqlite page cache
** subsystem.  The page cache subsystem reads and writes a file a page
** at a time and provides a journal for rollback.
**
** @(#) $Id: pager.h,v 1.16 2002/03/05 12:41:20 drh Exp $
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
** 此头文件定义了sqlite页面缓存子系统的接口。
** 页面缓存子系统一次读取和写入一个页面的文件，并提供用于回滚的日志。
*/

/*
** The size of one page
**
** You can change this value to another (reasonable) power of two
** such as 512, 2048, 4096, or 8192 and things will still work.  But
** experiments show that a page size of 1024 gives the best speed.
** (The speed differences are minimal.)
**
** 一个页面的大小
**
** 您可以将此值更改为另一个（合理的）2的幂，
** 例如512、2048、4096或8192，事情仍然可以正常工作。
** 但是实验表明，1024的页面大小可以提供最佳速度。
** （速度差异很小。）
*/
#define SQLITE_PAGE_SIZE 1024

/*
** Maximum number of pages in one database.  (This is a limitation of
** imposed by 4GB files size limits.)
**
** 一个数据库中的最大页面数。（这是由4GB文件大小限制施加的限制。）
*/
#define SQLITE_MAX_PAGE 1073741823

/*
** The type used to represent a page number.  The first page in a file
** is called page 1.  0 is used to represent "not a page".
**
** 用于表示页码的类型。文件中的第一页称为第1页。0用于表示“不是页面”。
*/
typedef unsigned int Pgno;

/*
** Each open file is managed by a separate instance of the "Pager" structure.
**
** 每个打开的文件都由“Pager”结构的单独实例管理。
*/
typedef struct Pager Pager;

/*
** See source code comments for a detailed description of the following
** routines:
**
** 有关以下例程的详细说明，请参阅源代码注释：
*/
int sqlitepager_open(Pager **ppPager,const char *zFilename,int nPage,int nEx);
void sqlitepager_set_destructor(Pager*, void(*)(void*));
void sqlitepager_set_cachesize(Pager*, int);
int sqlitepager_close(Pager *pPager);
int sqlitepager_get(Pager *pPager, Pgno pgno, void **ppPage);
void *sqlitepager_lookup(Pager *pPager, Pgno pgno);
int sqlitepager_ref(void*);
int sqlitepager_unref(void*);
Pgno sqlitepager_pagenumber(void*);
int sqlitepager_write(void*);
int sqlitepager_iswriteable(void*);
int sqlitepager_pagecount(Pager*);
int sqlitepager_begin(void*);
int sqlitepager_commit(Pager*);
int sqlitepager_rollback(Pager*);
int sqlitepager_isreadonly(Pager*);
int sqlitepager_ckpt_begin(Pager*);
int sqlitepager_ckpt_commit(Pager*);
int sqlitepager_ckpt_rollback(Pager*);
void sqlitepager_dont_rollback(void*);
void sqlitepager_dont_write(Pager*, Pgno);
int *sqlitepager_stats(Pager*);

#ifdef SQLITE_TEST
void sqlitepager_refdump(Pager*);
int pager_refinfo_enable;
#endif
