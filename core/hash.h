/*
** 2001 September 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This is the header file for the generic hash-table implemenation
** used in SQLite.
**
** $Id: hash.h,v 1.5 2002/06/08 23:25:09 drh Exp $
**
** 2001年9月22日
**
** 作者放弃此源代码的版权。作为法律声明的替代，这里有一个祝福：
**
**    愿你行善而不作恶。
**    愿你宽恕自己并宽恕他人。
**    愿你自由分享，索取不超所予。
**
** *************************************************************************
** 这是SQLite中使用的通用哈希表实现的头文件。
*/
#ifndef _SQLITE_HASH_H_
#define _SQLITE_HASH_H_

/* Forward declarations of structures.
** 结构的前向声明。 */
typedef struct Hash Hash;
typedef struct HashElem HashElem;

/* A complete hash table is an instance of the following structure.
** The internals of this structure are intended to be opaque -- client
** code should not attempt to access or modify the fields of this structure
** directly.  Change this structure only by using the routines below.
** However, many of the "procedures" and "functions" for modifying and
** accessing this structure are really macros, so we can't really make
** this structure opaque.
**
** 完整的哈希表是以下结构的一个实例。
** 此结构的内部旨在是不透明的——客户端代码不应尝试直接访问或修改此结构的字段。
** 仅使用下面的例程更改此结构。
** 但是，许多用于修改和访问此结构的“过程”和“函数”实际上是宏，
** 因此我们无法真正使此结构不透明。
*/
struct Hash {
    char keyClass;          /* SQLITE_HASH_INT, _POINTER, _STRING, _BINARY */
    char copyKey;           /* True if copy of key made on insert */
    int count;              /* Number of entries in this table */
    HashElem *first;        /* The first element of the array */
    int htsize;             /* Number of buckets in the hash table */
    struct _ht {            /* the hash table */
        int count;               /* Number of entries with this hash */
        HashElem *chain;         /* Pointer to first entry with this hash */
    } *ht;
    /* 如果在插入时制作了键的副本，则为True */
    /* 此表中的条目数 */
    /* 数组的第一个元素 */
    /* 哈希表中的桶数 */
    /* 哈希表 */
    /* 具有此哈希的条目数 */
    /* 指向具有此哈希的第一个条目的指针 */
};

/* Each element in the hash table is an instance of the following
** structure.  All elements are stored on a single doubly-linked list.
**
** Again, this structure is intended to be opaque, but it can't really
** be opaque because it is used by macros.
**
** 哈希表中的每个元素都是以下结构的一个实例。
** 所有元素都存储在单个双向链表上。
**
** 同样，此结构旨在是不透明的，但它不能真正不透明，因为它被宏使用。
*/
struct HashElem {
    HashElem *next, *prev;   /* Next and previous elements in the table */
    void *data;              /* Data associated with this element */
    void *pKey; int nKey;    /* Key associated with this element */
    /* 表中的下一个和上一个元素 */
    /* 与此元素关联的数据 */
    /* 与此元素关联的键 */
};

/*
** There are 4 different modes of operation for a hash table:
**
**   SQLITE_HASH_INT         nKey is used as the key and pKey is ignored.
**
**   SQLITE_HASH_POINTER     pKey is used as the key and nKey is ignored.
**
**   SQLITE_HASH_STRING      pKey points to a string that is nKey bytes long
**                           (including the null-terminator, if any).  Case
**                           is ignored in comparisons.
**
**   SQLITE_HASH_BINARY      pKey points to binary data nKey bytes long.
**                           memcmp() is used to compare keys.
**
** A copy of the key is made for SQLITE_HASH_STRING and SQLITE_HASH_BINARY
** if the copyKey parameter to HashInit is 1.
**
** 哈希表有4种不同的操作模式：
**
**   SQLITE_HASH_INT         nKey用作键，pKey被忽略。
**
**   SQLITE_HASH_POINTER     pKey用作键，nKey被忽略。
**
**   SQLITE_HASH_STRING      pKey指向一个nKey字节长的字符串
**                           （包括空终止符，如果有）。
**                           在比较中忽略大小写。
**
**   SQLITE_HASH_BINARY      pKey指向nKey字节长的二进制数据。
**                           使用memcmp()比较键。
**
** 如果HashInit的copyKey参数为1，则为SQLITE_HASH_STRING和SQLITE_HASH_BINARY
** 制作键的副本。
*/
#define SQLITE_HASH_INT       1
#define SQLITE_HASH_POINTER   2
#define SQLITE_HASH_STRING    3
#define SQLITE_HASH_BINARY    4

/*
** Access routines.  To delete, insert a NULL pointer.
**
** 访问例程。要删除，请插入NULL指针。
*/
void sqliteHashInit(Hash*, int keytype, int copyKey);
void *sqliteHashInsert(Hash*, const void *pKey, int nKey, void *pData);
void *sqliteHashFind(const Hash*, const void *pKey, int nKey);
void sqliteHashClear(Hash*);

static int strHash(const void *pKey, int nKey);

/*
** Macros for looping over all elements of a hash table.  The idiom is
** like this:
**
**   Hash h;
**   HashElem *p;
**   ...
**   for(p=sqliteHashFirst(&h); p; p=sqliteHashNext(p)){
**     SomeStructure *pData = sqliteHashData(p);
**     // do something with pData
**   }
**
** 用于循环遍历哈希表所有元素的宏。用法如下：
**
**   Hash h;
**   HashElem *p;
**   ...
**   for(p=sqliteHashFirst(&h); p; p=sqliteHashNext(p)){
**     SomeStructure *pData = sqliteHashData(p);
**     // 对pData做些什么
**   }
*/
#define sqliteHashFirst(H)  ((H)->first)
#define sqliteHashNext(E)   ((E)->next)
#define sqliteHashData(E)   ((E)->data)
#define sqliteHashKey(E)    ((E)->pKey)
#define sqliteHashKeysize(E) ((E)->nKey)

/*
** Number of entries in a hash table
**
** 哈希表中的条目数
*/
#define sqliteHashCount(H)  ((H)->count)

#endif /* _SQLITE_HASH_H_ */
