/*
** 2001 September 16
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains code that is specific to particular operating
** systems.  The purpose of this file is to provide a uniform abstraction
** on which the rest of SQLite can operate.
**
** 2001年9月16日
**
** 作者放弃此源代码的版权。作为法律声明的替代，这里有一个祝福：
**
**    愿你行善而不作恶。
**    愿你宽恕自己并宽恕他人。
**    愿你自由分享，索取不超所予。
**
** ******************************************************************************
**
** 此文件包含特定于特定操作系统的代码。
** 此文件的目的是提供一个统一的抽象层，SQLite的其余部分可以在其上运行。
*/
#include "sqliteInt.h"
#include "os.h"
#include "sqlite.h"
#include "hash.h"
#include "random.h"
#include <assert.h>
#include <stdio.h>
#if OS_UNIX
# include <unistd.h>
# include <fcntl.h>
# include <sys/stat.h>
# include <time.h>
#endif
#if OS_WIN
# include <winbase.h>
#endif

/*
** Macros for performance tracing.  Normally turned off
**
** 用于性能跟踪的宏。通常关闭
*/
#if 0
static int last_page = 0;
#define SEEK(X)     last_page=(X)
#define TRACE1(X)   fprintf(stderr,X)
#define TRACE2(X,Y) fprintf(stderr,X,Y)
#else
#define SEEK(X)
#define TRACE1(X)
#define TRACE2(X,Y)
#endif


#if OS_UNIX
/*
** Here is the dirt on POSIX advisory locks:  ANSI STD 1003.1 (1996)
** section 6.5.2.2 lines 483 through 490 specify that when a process
** sets or clears a lock, that operation overrides any prior locks set
** by the same process.  It does not explicitly say so, but this implies
** that it overrides locks set by the same process using a different
** file descriptor.  Consider this test case:
**
**       int fd1 = open("./file1", O_RDWR|O_CREAT, 0644);
**       int fd2 = open("./file2", O_RDWR|O_CREAT, 0644);
**
** Suppose ./file1 and ./file2 are really be the same file (because
** one is a hard or symbolic link to the other) then if you set
** an exclusive lock on fd1, then try to get an exclusive lock
** on fd2, it works.  I would have expected the second lock to
** fail since there was already a lock on the file due to fd1.
** But not so.  Since both locks came from the same process, the
** second overrides the first, even though they were on different
** file descriptors opened on different file names.
**
** Bummer.  If you ask me, this is broken.  Badly broken.  It means
** that we cannot use POSIX locks to synchronize file access among
** competing threads of the same process.  POSIX locks will work fine
** to synchronize access for threads in separate processes, but not
** threads within the same process.
**
** To work around the problem, SQLite has to manage file locks internally
** on its own.  Whenever a new database is opened, we have to find the
** specific inode of the database file (the inode is determined by the
** st_dev and st_ino fields of the stat structure that fstat() fills in)
** and check for locks already existing on that inode.  When locks are
** created or removed, we have to look at our own internal record of the
** locks to see if another thread has previously set a lock on that same
** inode.
**
** The OsFile structure for POSIX is no longer just an integer file
** descriptor.  It is now a structure that holds the integer file
** descriptor and a pointer to a structure that describes the internal
** locks on the corresponding inode.  There is one locking structure
** per inode, so if the same inode is opened twice, both OsFile structures
** point to the same locking structure.  The locking structure keeps
** a reference count (so we will know when to delete it) and a "cnt"
** field that tells us its internal lock status.  cnt==0 means the
** file is unlocked.  cnt==-1 means the file has an exclusive lock.
** cnt>0 means there are cnt shared locks on the file.
**
** Any attempt to lock or unlock a file first checks the locking
** structure.  The fcntl() system call is only invoked to set a
** POSIX lock if the internal lock structure transitions between
** a locked and an unlocked state.
**
** 关于POSIX咨询锁的内幕：ANSI STD 1003.1 (1996)
** 第6.5.2.2节第483至490行规定，当进程设置或清除锁时，
** 该操作会覆盖同一进程设置的任何先前锁。它没有明确这么说，但这暗示
** 它会覆盖同一进程使用不同文件描述符设置的锁。考虑这个测试用例：
**
**       int fd1 = open("./file1", O_RDWR|O_CREAT, 0644);
**       int fd2 = open("./file2", O_RDWR|O_CREAT, 0644);
**
** 假设./file1和./file2实际上是同一个文件（因为一个是另一个的硬链接或符号链接），
** 那么如果你在fd1上设置排他锁，然后尝试在fd2上获取排他锁，它是有效的。
** 我原本期望第二个锁会失败，因为fd1已经在文件上加了锁。
** 但事实并非如此。由于两个锁都来自同一个进程，第二个锁覆盖了第一个锁，
** 即使它们是在不同文件名上打开的不同文件描述符。
**
** 糟糕。如果你问我，这是坏的。坏得很厉害。这意味着
** 我们不能使用POSIX锁来同步同一进程的竞争线程之间的文件访问。
** POSIX锁可以很好地同步不同进程中线程的访问，但不能同步同一进程内的线程。
**
** 为了解决这个问题，SQLite必须在内部自行管理文件锁。
** 每当打开一个新的数据库时，我们必须找到数据库文件的特定inode
** （inode由fstat()填充的stat结构的st_dev和st_ino字段确定），
** 并检查该inode上是否已存在锁。当创建或删除锁时，
** 我们必须查看我们自己的内部锁记录，看看另一个线程是否之前在同一个inode上设置了锁。
**
** POSIX的OsFile结构不再只是一个整数文件描述符。
** 它现在是一个结构，包含整数文件描述符和一个指向描述相应inode上内部锁的结构的指针。
** 每个inode有一个锁定结构，因此如果同一个inode被打开两次，两个OsFile结构
** 都指向同一个锁定结构。锁定结构保留引用计数（以便我们知道何时删除它）
** 和一个告诉我们其内部锁定状态的“cnt”字段。cnt==0表示文件已解锁。
** cnt==-1表示文件具有排他锁。cnt>0表示文件上有cnt个共享锁。
**
** 任何锁定或解锁文件的尝试都会首先检查锁定结构。
** 只有当内部锁定结构在锁定和解锁状态之间转换时，才会调用fcntl()系统调用来设置POSIX锁。
*/

/*
** An instance of the following structure serves as the key used
** to locate a particular lockInfo structure given its inode.
**
** 以下结构的实例用作键，用于给定inode定位特定的lockInfo结构。
*/
struct inodeKey {
  dev_t dev;   /* Device number */
  ino_t ino;   /* Inode number */
};

/*
** An instance of the following structure is allocated for each inode.
** A single inode can have multiple file descriptors, so each OsFile
** structure contains a pointer to an instance of this object and this
** object keeps a count of the number of OsFiles pointing to it.
**
** 为每个inode分配一个以下结构的实例。
** 单个inode可以有多个文件描述符，因此每个OsFile结构都包含一个指向此对象实例的指针，
** 并且此对象保留指向它的OsFile数量的计数。
*/
struct lockInfo {
  struct inodeKey key;  /* The lookup key */
  int cnt;              /* 0: unlocked.  -1: write lock.  1...: read lock. */
  int nRef;             /* Number of pointers to this structure */
};

/*
** This hash table maps inodes (in the form of inodeKey structures) into
** pointers to lockInfo structures.
**
** 此哈希表将inode（以inodeKey结构的形式）映射到指向lockInfo结构的指针。
*/
static Hash lockHash = { SQLITE_HASH_BINARY, 0, 0, 0, 0, 0 };

/*
** Given a file descriptor, locate a lockInfo structure that describes
** that file descriptor.  Create a new one if necessary.  NULL might
** be returned if malloc() fails.
**
** 给定一个文件描述符，找到描述该文件描述符的lockInfo结构。
** 如有必要，创建一个新的。如果malloc()失败，可能会返回NULL。
*/
static struct lockInfo *findLockInfo(int fd){
  int rc;
  struct inodeKey key;
  struct stat statbuf;
  struct lockInfo *pInfo;
  rc = fstat(fd, &statbuf);
  if( rc!=0 ) return 0;
  memset(&key, 0, sizeof(key));
  key.dev = statbuf.st_dev;
  key.ino = statbuf.st_ino;
  pInfo = (struct lockInfo*)sqliteHashFind(&lockHash, &key, sizeof(key));
  if( pInfo==0 ){
    struct lockInfo *pOld;
    pInfo = malloc( sizeof(*pInfo) );
    memset(pInfo, 0, sizeof(*pInfo));
    if( pInfo==0 ) return 0;
    pInfo->key = key;
    pInfo->nRef = 1;
    pInfo->cnt = 0;
    pOld = sqliteHashInsert(&lockHash, &pInfo->key, sizeof(key), pInfo);
    if( pOld!=0 ){
      assert( pOld==pInfo );
      sqliteFree(pInfo);
      pInfo = 0;
    }
  }else{
    pInfo->nRef++;
  }
  return pInfo;
}

/*
** Release a lockInfo structure previously allocated by findLockInfo().
**
** 释放之前由findLockInfo()分配的lockInfo结构。
*/
static void releaseLockInfo(struct lockInfo *pInfo){
  pInfo->nRef--;
  if( pInfo->nRef==0 ){
    sqliteHashInsert(&lockHash, &pInfo->key, sizeof(pInfo->key), 0);
    sqliteFree(pInfo);
  }
}
#endif  /** POSIX advisory lock work-around **/

/*
** If we compile with the SQLITE_TEST macro set, then the following block
** of code will give us the ability to simulate a disk I/O error.  This
** is used for testing the I/O recovery logic.
**
** 如果我们在设置了SQLITE_TEST宏的情况下进行编译，则以下代码块将使我们能够模拟磁盘I/O错误。
** 这用于测试I/O恢复逻辑。
*/
#ifdef SQLITE_TEST
int sqlite_io_error_pending = 0;
#define SimulateIOError(A)  \
   if( sqlite_io_error_pending ) \
     if( sqlite_io_error_pending-- == 1 ){ local_ioerr(); return A; }
static void local_ioerr(){
  sqlite_io_error_pending = 0;  /* Really just a place to set a breakpoint */
}
#else
#define SimulateIOError(A)
#endif


/*
** Delete the named file
**
** 删除指定文件
*/
int sqliteOsDelete(const char *zFilename){
#if OS_UNIX
    unlink(zFilename);
#endif
#if OS_WIN
    DeleteFile(zFilename);
#endif
    return SQLITE_OK;
}

/*
** Return TRUE if the named file exists.
**
** 如果指定文件存在，则返回TRUE。
*/
int sqliteOsFileExists(const char *zFilename){
#if OS_UNIX
    return access(zFilename, 0)==0;
#endif
#if OS_WIN
    return GetFileAttributes(zFilename) != 0xffffffff;
#endif
}


/*
** Attempt to open a file for both reading and writing.  If that
** fails, try opening it read-only.  If the file does not exist,
** try to create it.
**
** On success, a handle for the open file is written to *id
** and *pReadonly is set to 0 if the file was opened for reading and
** writing or 1 if the file was opened read-only.  The function returns
** SQLITE_OK.
**
** On failure, the function returns SQLITE_CANTOPEN and leaves
** *pResulst and *pReadonly unchanged.
**
** 尝试打开一个文件进行读写。如果失败，尝试以只读方式打开它。
** 如果文件不存在，尝试创建它。
**
** 成功时，将打开文件的句柄写入*id，如果文件是为读写打开的，则将*pReadonly设置为0，
** 如果文件是以只读方式打开的，则设置为1。该函数返回SQLITE_OK。
**
** 失败时，该函数返回SQLITE_CANTOPEN，并保持*pResulst和*pReadonly不变。
*/
int sqliteOsOpenReadWrite(
        const char *zFilename,
        OsFile *id,
        int *pReadonly
){
#if OS_UNIX
    id->fd = open(zFilename, O_RDWR|O_CREAT, 0644);
  if( id->fd<0 ){
    id->fd = open(zFilename, O_RDONLY);
    if( id->fd<0 ){
      return SQLITE_CANTOPEN;
    }
    *pReadonly = 1;
  }else{
    *pReadonly = 0;
  }
  sqliteOsEnterMutex();
  id->pLock = findLockInfo(id->fd);
  sqliteOsLeaveMutex();
  if( id->pLock==0 ){
    close(id->fd);
    return SQLITE_NOMEM;
  }
  id->locked = 0;
  return SQLITE_OK;
#endif
#if OS_WIN
    HANDLE h = CreateFile(zFilename,
     GENERIC_READ | GENERIC_WRITE,
     FILE_SHARE_READ | FILE_SHARE_WRITE,
     NULL,
     OPEN_ALWAYS,
     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
     NULL
  );
  if( h==INVALID_HANDLE_VALUE ){
    h = CreateFile(zFilename,
       GENERIC_READ,
       FILE_SHARE_READ,
       NULL,
       OPEN_ALWAYS,
       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
       NULL
    );
    if( h==INVALID_HANDLE_VALUE ){
      return SQLITE_CANTOPEN;
    }
    *pReadonly = 1;
  }else{
    *pReadonly = 0;
  }
  id->h = h;
  id->locked = 0;
  return SQLITE_OK;
#endif
}


/*
** Attempt to open a new file for exclusive access by this process.
** The file will be opened for both reading and writing.  To avoid
** a potential security problem, we do not allow the file to have
** previously existed.  Nor do we allow the file to be a symbolic
** link.
**
** If delFlag is true, then make arrangements to automatically delete
** the file when it is closed.
**
** On success, write the file handle into *id and return SQLITE_OK.
**
** On failure, return SQLITE_CANTOPEN.
**
** 尝试打开一个新文件以供此进程独占访问。
** 该文件将打开用于读写。为了避免潜在的安全问题，我们不允许文件之前已存在。
** 我们也不允许文件是符号链接。
**
** 如果delFlag为true，则安排在关闭文件时自动删除该文件。
**
** 成功时，将文件句柄写入*id并返回SQLITE_OK。
**
** 失败时，返回SQLITE_CANTOPEN。
*/
int sqliteOsOpenExclusive(const char *zFilename, OsFile *id, int delFlag){
#if OS_UNIX
    if( access(zFilename, 0)==0 ){
    return SQLITE_CANTOPEN;
  }
#ifndef O_NOFOLLOW
# define O_NOFOLLOW 0
#endif
  id->fd = open(zFilename, O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW, 0600);
  if( id->fd<0 ){
    return SQLITE_CANTOPEN;
  }
  sqliteOsEnterMutex();
  id->pLock = findLockInfo(id->fd);
  sqliteOsLeaveMutex();
  if( id->pLock==0 ){
    close(id->fd);
    unlink(zFilename);
    return SQLITE_NOMEM;
  }
  id->locked = 0;
  if( delFlag ){
    unlink(zFilename);
  }
  return SQLITE_OK;
#endif
#if OS_WIN
    HANDLE h;
  int fileflags;
  if( delFlag ){
    fileflags = FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_RANDOM_ACCESS
                     | FILE_FLAG_DELETE_ON_CLOSE;
  }else{
    fileflags = FILE_FLAG_RANDOM_ACCESS;
  }
  h = CreateFile(zFilename,
     GENERIC_READ | GENERIC_WRITE,
     0,
     NULL,
     CREATE_ALWAYS,
     fileflags,
     NULL
  );
  if( h==INVALID_HANDLE_VALUE ){
    return SQLITE_CANTOPEN;
  }
  id->h = h;
  id->locked = 0;
  return SQLITE_OK;
#endif
}

/*
** Attempt to open a new file for read-only access.
**
** On success, write the file handle into *id and return SQLITE_OK.
**
** On failure, return SQLITE_CANTOPEN.
**
** 尝试打开一个新文件以进行只读访问。
**
** 成功时，将文件句柄写入*id并返回SQLITE_OK。
**
** 失败时，返回SQLITE_CANTOPEN。
*/
int sqliteOsOpenReadOnly(const char *zFilename, OsFile *id){
#if OS_UNIX
    id->fd = open(zFilename, O_RDONLY);
  if( id->fd<0 ){
    return SQLITE_CANTOPEN;
  }
  sqliteOsEnterMutex();
  id->pLock = findLockInfo(id->fd);
  sqliteOsLeaveMutex();
  if( id->pLock==0 ){
    close(id->fd);
    return SQLITE_NOMEM;
  }
  id->locked = 0;
  return SQLITE_OK;
#endif
#if OS_WIN
    HANDLE h = CreateFile(zFilename,
     GENERIC_READ,
     0,
     NULL,
     OPEN_EXISTING,
     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
     NULL
  );
  if( h==INVALID_HANDLE_VALUE ){
    return SQLITE_CANTOPEN;
  }
  id->h = h;
  id->locked = 0;
  return SQLITE_OK;
#endif
}

/*
** Create a temporary file name in zBuf.  zBuf must be big enough to
** hold at least SQLITE_TEMPNAME_SIZE characters.
**
** 在zBuf中创建一个临时文件名。zBuf必须足够大，至少能容纳SQLITE_TEMPNAME_SIZE个字符。
*/
int sqliteOsTempFileName(char *zBuf){
#if OS_UNIX
    static const char *azDirs[] = {
     ".",
     "/var/tmp",
     "/usr/tmp",
     "/tmp",
  };
  static char zChars[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789";
  int i, j;
  struct stat buf;
  const char *zDir = ".";
  for(i=0; i<sizeof(azDirs)/sizeof(azDirs[0]); i++){
    if( stat(azDirs[i], &buf) ) continue;
    if( !S_ISDIR(buf.st_mode) ) continue;
    if( access(azDirs[i], 07) ) continue;
    zDir = azDirs[i];
    break;
  }
  do{
    sprintf(zBuf, "%s/sqlite_", zDir);
    j = strlen(zBuf);
    for(i=0; i<15; i++){
      int n = sqliteRandomByte() % (sizeof(zChars)-1);
      zBuf[j++] = zChars[n];
    }
    zBuf[j] = 0;
  }while( access(zBuf,0)==0 );
#endif
#if OS_WIN
    static char zChars[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789";
  int i, j;
  char zTempPath[SQLITE_TEMPNAME_SIZE];
  GetTempPath(SQLITE_TEMPNAME_SIZE-30, zTempPath);
  for(i=strlen(zTempPath); i>0 && zTempPath[i-1]=='\\'; i--){}
  zTempPath[i] = 0;
  for(;;){
    sprintf(zBuf, "%s\\sqlite_", zTempPath);
    j = strlen(zBuf);
    for(i=0; i<15; i++){
      int n = sqliteRandomByte() % sizeof(zChars);
      zBuf[j++] = zChars[n];
    }
    zBuf[j] = 0;
    if( !sqliteOsFileExists(zBuf) ) break;
  }
#endif
    return SQLITE_OK;
}

/*
** Close a file
**
** 关闭文件
*/
int sqliteOsClose(OsFile *id){
#if OS_UNIX
    close(id->fd);
  sqliteOsEnterMutex();
  releaseLockInfo(id->pLock);
  sqliteOsLeaveMutex();
  return SQLITE_OK;
#endif
#if OS_WIN
    CloseHandle(id->h);
  return SQLITE_OK;
#endif
}

/*
** Read data from a file into a buffer.  Return SQLITE_OK if all
** bytes were read successfully and SQLITE_IOERR if anything goes
** wrong.
**
** 将数据从文件读取到缓冲区中。如果所有字节都成功读取，则返回SQLITE_OK，
** 如果出现任何问题，则返回SQLITE_IOERR。
*/
int sqliteOsRead(OsFile *id, void *pBuf, int amt){
#if OS_UNIX
    int got;
  SimulateIOError(SQLITE_IOERR);
  TRACE2("READ %d\n", last_page);
  got = read(id->fd, pBuf, amt);
  if( got<0 ) got = 0;
  return got==amt ? SQLITE_OK : SQLITE_IOERR;
#endif
#if OS_WIN
    DWORD got;
  SimulateIOError(SQLITE_IOERR);
  if( !ReadFile(id->h, pBuf, amt, &got, 0) ){
    got = 0;
  }
  return got==amt ? SQLITE_OK : SQLITE_IOERR;
#endif
}

/*
** Write data from a buffer into a file.  Return SQLITE_OK on success
** or some other error code on failure.
**
** 将数据从缓冲区写入文件。成功时返回SQLITE_OK，失败时返回其他错误代码。
*/
int sqliteOsWrite(OsFile *id, const void *pBuf, int amt){
#if OS_UNIX
    int wrote;
  SimulateIOError(SQLITE_IOERR);
  TRACE2("WRITE %d\n", last_page);
  wrote = write(id->fd, pBuf, amt);
  if( wrote<amt ) return SQLITE_FULL;
  return SQLITE_OK;
#endif
#if OS_WIN
    DWORD wrote;
  SimulateIOError(SQLITE_IOERR);
  if( !WriteFile(id->h, pBuf, amt, &wrote, 0) || (int)wrote<amt ){
    return SQLITE_FULL;
  }
  return SQLITE_OK;
#endif
}

/*
** Move the read/write pointer in a file.
**
** 移动文件中的读/写指针。
*/
int sqliteOsSeek(OsFile *id, int offset){
    SEEK(offset/1024 + 1);
#if OS_UNIX
    lseek(id->fd, offset, SEEK_SET);
  return SQLITE_OK;
#endif
#if OS_WIN
    SetFilePointer(id->h, offset, 0, FILE_BEGIN);
  return SQLITE_OK;
#endif
}

/*
** Make sure all writes to a particular file are committed to disk.
**
** 确保对特定文件的所有写入都提交到磁盘。
*/
int sqliteOsSync(OsFile *id){
    SimulateIOError(SQLITE_IOERR);
    TRACE1("SYNC\n");
#if OS_UNIX
    return fsync(id->fd)==0 ? SQLITE_OK : SQLITE_IOERR;
#endif
#if OS_WIN
    return FlushFileBuffers(id->h) ? SQLITE_OK : SQLITE_IOERR;
#endif
}

/*
** Truncate an open file to a specified size
**
** 将打开的文件截断为指定大小
*/
int sqliteOsTruncate(OsFile *id, int nByte){
    SimulateIOError(SQLITE_IOERR);
#if OS_UNIX
    return ftruncate(id->fd, nByte)==0 ? SQLITE_OK : SQLITE_IOERR;
#endif
#if OS_WIN
    SetFilePointer(id->h, nByte, 0, FILE_BEGIN);
  SetEndOfFile(id->h);
  return SQLITE_OK;
#endif
}

/*
** Determine the current size of a file in bytes
**
** 确定文件的当前大小（以字节为单位）
*/
int sqliteOsFileSize(OsFile *id, int *pSize){
#if OS_UNIX
    struct stat buf;
  SimulateIOError(SQLITE_IOERR);
  if( fstat(id->fd, &buf)!=0 ){
    return SQLITE_IOERR;
  }
  *pSize = buf.st_size;
  return SQLITE_OK;
#endif
#if OS_WIN
    SimulateIOError(SQLITE_IOERR);
  *pSize = GetFileSize(id->h, 0);
  return SQLITE_OK;
#endif
}


/*
** Change the status of the lock on the file "id" to be a readlock.
** If the file was write locked, then this reduces the lock to a read.
** If the file was read locked, then this acquires a new read lock.
**
** Return SQLITE_OK on success and SQLITE_BUSY on failure.
**
** 将文件“id”上的锁状态更改为读锁。
** 如果文件被写锁定，则这会将锁降级为读锁。
** 如果文件被读锁定，则这会获取一个新的读锁。
**
** 成功时返回SQLITE_OK，失败时返回SQLITE_BUSY。
*/
int sqliteOsReadLock(OsFile *id){
#if OS_UNIX
    int rc;
  sqliteOsEnterMutex();
  if( id->pLock->cnt>0 ){
    if( !id->locked ){
      id->pLock->cnt++;
      id->locked = 1;
    }
    rc = SQLITE_OK;
  }else if( id->locked || id->pLock->cnt==0 ){
    struct flock lock;
    lock.l_type = F_RDLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = lock.l_len = 0L;
    if( fcntl(id->fd, F_SETLK, &lock)!=0 ){
      rc = SQLITE_BUSY;
    }else{
      rc = SQLITE_OK;
      id->pLock->cnt = 1;
      id->locked = 1;
    }
  }else{
    rc = SQLITE_BUSY;
  }
  sqliteOsLeaveMutex();
  return rc;
#endif
#if OS_WIN
    int rc;
  if( id->locked ){
    rc = SQLITE_OK;
  }else if( LockFile(id->h, 0, 0, 1024, 0) ){
    rc = SQLITE_OK;
    id->locked = 1;
  }else{
    rc = SQLITE_BUSY;
  }
  return rc;
#endif
}

/*
** Change the lock status to be an exclusive or write lock.  Return
** SQLITE_OK on success and SQLITE_BUSY on a failure.
**
** 将锁状态更改为排他锁或写锁。成功时返回SQLITE_OK，失败时返回SQLITE_BUSY。
*/
int sqliteOsWriteLock(OsFile *id){
#if OS_UNIX
    int rc;
  sqliteOsEnterMutex();
  if( id->pLock->cnt==0 || (id->pLock->cnt==1 && id->locked==1) ){
    struct flock lock;
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = lock.l_len = 0L;
    if( fcntl(id->fd, F_SETLK, &lock)!=0 ){
      rc = SQLITE_BUSY;
    }else{
      rc = SQLITE_OK;
      id->pLock->cnt = -1;
      id->locked = 1;
    }
  }else{
    rc = SQLITE_BUSY;
  }
  sqliteOsLeaveMutex();
  return rc;
#endif
#if OS_WIN
    int rc;
  if( id->locked ){
    rc = SQLITE_OK;
  }else if( LockFile(id->h, 0, 0, 1024, 0) ){
    rc = SQLITE_OK;
    id->locked = 1;
  }else{
    rc = SQLITE_BUSY;
  }
  return rc;
#endif
}

/*
** Unlock the given file descriptor.  If the file descriptor was
** not previously locked, then this routine is a no-op.
**
** 解锁给定的文件描述符。如果文件描述符之前未被锁定，则此例程为无操作。
*/
int sqliteOsUnlock(OsFile *id){
#if OS_UNIX
    int rc;
  if( !id->locked ) return SQLITE_OK;
  sqliteOsEnterMutex();
  assert( id->pLock->cnt!=0 );
  if( id->pLock->cnt>1 ){
    id->pLock->cnt--;
    rc = SQLITE_OK;
  }else{
    struct flock lock;
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = lock.l_len = 0L;
    if( fcntl(id->fd, F_SETLK, &lock)!=0 ){
      rc = SQLITE_BUSY;
    }else{
      rc = SQLITE_OK;
      id->pLock->cnt = 0;
    }
  }
  sqliteOsLeaveMutex();
  id->locked = 0;
  return rc;
#endif
#if OS_WIN
    int rc;
  if( !id->locked ){
    rc = SQLITE_OK;
  }else if( UnlockFile(id->h, 0, 0, 1024, 0) ){
    rc = SQLITE_OK;
    id->locked = 0;
  }else{
    rc = SQLITE_BUSY;
  }
  return rc;
#endif
}

/*
** Get information to seed the random number generator.
**
** 获取用于播种随机数生成器的信息。
*/
int sqliteOsRandomSeed(char *zBuf){
    static int once = 1;
#if OS_UNIX
    int pid;
  time((time_t*)zBuf);
  pid = getpid();
  memcpy(&zBuf[sizeof(time_t)], &pid, sizeof(pid));
#endif
#if OS_WIN
    GetSystemTime((LPSYSTEMTIME)zBuf);
#endif
    if( once ){
        int seed;
        memcpy(&seed, zBuf, sizeof(seed));
        srand(seed);
        once = 0;
    }
    return SQLITE_OK;
}

/*
** Sleep for a little while.  Return the amount of time slept.
**
** 睡一会儿。返回睡眠的时间量。
*/
int sqliteOsSleep(int ms){
#if OS_UNIX
    #if defined(HAVE_USLEEP) && HAVE_USLEEP
  usleep(ms*1000);
  return ms;
#else
  sleep((ms+999)/1000);
  return 1000*((ms+999)/1000);
#endif
#endif
#if OS_WIN
    Sleep(ms);
  return ms;
#endif
}

/*
** Macros used to determine whether or not to use threads.  The
** SQLITE_UNIX_THREADS macro is defined if we are synchronizing for
** Posix threads and SQLITE_W32_THREADS is defined if we are
** synchronizing using Win32 threads.
**
** 用于确定是否使用线程的宏。
** 如果我们正在为Posix线程进行同步，则定义SQLITE_UNIX_THREADS宏；
** 如果我们正在使用Win32线程进行同步，则定义SQLITE_W32_THREADS。
*/
#if OS_UNIX && defined(THREADSAFE) && THREADSAFE
# include <pthread.h>
# define SQLITE_UNIX_THREADS 1
#endif
#if OS_WIN && defined(THREADSAFE) && THREADSAFE
# define SQLITE_W32_THREADS 1
#endif

/*
** Static variables used for thread synchronization
**
** 用于线程同步的静态变量
*/
static int inMutex = 0;
#ifdef SQLITE_UNIX_THREADS
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
#ifdef SQLITE_W32_THREADS
static CRITICAL_SECTION cs;
#endif

/*
** The following pair of routine implement mutual exclusion for
** multi-threaded processes.  Only a single thread is allowed to
** executed code that is surrounded by EnterMutex() and LeaveMutex().
**
** SQLite uses only a single Mutex.  There is not much critical
** code and what little there is executes quickly and without blocking.
**
** 以下一对例程实现了多线程进程的互斥。
** 仅允许单个线程执行由EnterMutex()和LeaveMutex()包围的代码。
**
** SQLite仅使用单个互斥锁。关键代码不多，而且仅有的少量代码执行速度很快且不会阻塞。
*/
void sqliteOsEnterMutex(){
#ifdef SQLITE_UNIX_THREADS
    pthread_mutex_lock(&mutex);
#endif
#ifdef SQLITE_W32_THREADS
    static int isInit = 0;
  while( !isInit ){
    static long lock = 0;
    if( InterlockedIncrement(&lock)==1 ){
      InitializeCriticalSection(&cs);
      isInit = 1;
    }else{
      Sleep(1);
    }
  }
  EnterCriticalSection(&cs);
#endif
    assert( !inMutex );
    inMutex = 1;
}
void sqliteOsLeaveMutex(){
    assert( inMutex );
    inMutex = 0;
#ifdef SQLITE_UNIX_THREADS
    pthread_mutex_unlock(&mutex);
#endif
#ifdef SQLITE_W32_THREADS
    LeaveCriticalSection(&cs);
#endif
}
