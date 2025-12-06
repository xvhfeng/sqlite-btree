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
** This header file defines the interface that the SQLite library
** presents to client programs.
**
** @(#) $Id: sqlite.h.in,v 1.31 2002/05/10 05:44:56 drh Exp $
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
** 此头文件定义了SQLite库向客户端程序提供的接口。
*/
#ifndef _SQLITE_H_
#define _SQLITE_H_
#include <stdarg.h>     /* Needed for the definition of va_list */
                        /* 需要用于定义 va_list */

/*
** The version of the SQLite library.
**
** SQLite库的版本。
*/
#define SQLITE_VERSION         "--VERS--"

/*
** Make sure we can call this stuff from C++.
**
** 确保我们可以从C++调用这些东西。
*/
#ifdef __cplusplus
extern "C" {
#endif

/*
** The version string is also compiled into the library so that a program
** can check to make sure that the lib*.a file and the *.h file are from
** the same version.
**
** 版本字符串也编译到库中，以便程序可以检查以确保lib*.a文件和*.h文件来自同一版本。
*/
extern const char sqlite_version[];


/*
** The following constant holds one of two strings, "UTF-8" or "iso8859",
** depending on which character encoding the SQLite library expects to
** see.  The character encoding makes a difference for the LIKE and GLOB
** operators and for the LENGTH() and SUBSTR() functions.
**
** 以下常量包含两个字符串之一，“UTF-8”或“iso8859”，
** 具体取决于SQLite库期望看到的字符编码。
** 字符编码对LIKE和GLOB运算符以及LENGTH()和SUBSTR()函数有影响。
*/
extern const char sqlite_encoding[];

/*
** Each open sqlite database is represented by an instance of the
** following opaque structure.
**
** 每个打开的sqlite数据库都由以下不透明结构的实例表示。
*/
typedef struct sqlite sqlite;

/*
** A function to open a new sqlite database.
**
** If the database does not exist and mode indicates write
** permission, then a new database is created.  If the database
** does not exist and mode does not indicate write permission,
** then the open fails, an error message generated (if errmsg!=0)
** and the function returns 0.
**
** If mode does not indicates user write permission, then the
** database is opened read-only.
**
** The Truth:  As currently implemented, all databases are opened
** for writing all the time.  Maybe someday we will provide the
** ability to open a database readonly.  The mode parameters is
** provide in anticipation of that enhancement.
**
** 打开一个新的sqlite数据库的函数。
**
** 如果数据库不存在且模式指示写权限，则创建一个新数据库。
** 如果数据库不存在且模式不指示写权限，则打开失败，生成错误消息（如果errmsg!=0），
** 并且函数返回0。
**
** 如果模式不指示用户写权限，则以只读方式打开数据库。
**
** 事实：按照目前的实现，所有数据库始终以写入方式打开。
** 也许有一天我们将提供以只读方式打开数据库的功能。
** 提供模式参数是为了预见这种增强。
*/
sqlite *sqlite_open(const char *filename, int mode, char **errmsg);

/*
** A function to close the database.
**
** Call this function with a pointer to a structure that was previously
** returned from sqlite_open() and the corresponding database will by closed.
**
** 关闭数据库的函数。
**
** 使用指向先前从sqlite_open()返回的结构的指针调用此函数，相应的数据库将被关闭。
*/
void sqlite_close(sqlite *);

/*
** The type for a callback function.
**
** 回调函数的类型。
*/
typedef int (*sqlite_callback)(void*,int,char**, char**);

/*
** A function to executes one or more statements of SQL.
**
** If one or more of the SQL statements are queries, then
** the callback function specified by the 3rd parameter is
** invoked once for each row of the query result.  This callback
** should normally return 0.  If the callback returns a non-zero
** value then the query is aborted, all subsequent SQL statements
** are skipped and the sqlite_exec() function returns the SQLITE_ABORT.
**
** The 4th parameter is an arbitrary pointer that is passed
** to the callback function as its first parameter.
**
** The 2nd parameter to the callback function is the number of
** columns in the query result.  The 3rd parameter to the callback
** is an array of strings holding the values for each column.
** The 4th parameter to the callback is an array of strings holding
** the names of each column.
**
** The callback function may be NULL, even for queries.  A NULL
** callback is not an error.  It just means that no callback
** will be invoked.
**
** If an error occurs while parsing or evaluating the SQL (but
** not while executing the callback) then an appropriate error
** message is written into memory obtained from malloc() and
** *errmsg is made to point to that message.  The calling function
** is responsible for freeing the memory that holds the error
** message.  If errmsg==NULL, then no error message is ever written.
**
** The return value is is SQLITE_OK if there are no errors and
** some other return code if there is an error.  The particular
** return value depends on the type of error.
**
** If the query could not be executed because a database file is
** locked or busy, then this function returns SQLITE_BUSY.  (This
** behavior can be modified somewhat using the sqlite_busy_handler()
** and sqlite_busy_timeout() functions below.)
**
** 执行一个或多个SQL语句的函数。
**
** 如果一个或多个SQL语句是查询，则对查询结果的每一行调用第3个参数指定的回调函数一次。
** 此回调通常应返回0。如果回调返回非零值，则查询中止，
** 跳过所有后续SQL语句，并且sqlite_exec()函数返回SQLITE_ABORT。
**
** 第4个参数是一个任意指针，作为其第一个参数传递给回调函数。
**
** 回调函数的第2个参数是查询结果中的列数。
** 回调的第3个参数是一个字符串数组，其中包含每列的值。
** 回调的第4个参数是一个字符串数组，其中包含每列的名称。
**
** 回调函数可以为NULL，即使对于查询也是如此。NULL回调不是错误。
** 它只是意味着不会调用任何回调。
**
** 如果在解析或评估SQL时发生错误（但在执行回调时没有），
** 则将适当的错误消息写入从malloc()获得的内存中，并使*errmsg指向该消息。
** 调用函数负责释放保存错误消息的内存。
** 如果errmsg==NULL，则永远不会写入任何错误消息。
**
** 如果没有错误，则返回值为SQLITE_OK，如果有错误，则为其他返回代码。
** 特定的返回值取决于错误的类型。
**
** 如果由于数据库文件被锁定或忙碌而无法执行查询，则此函数返回SQLITE_BUSY。
** （可以使用下面的sqlite_busy_handler()和sqlite_busy_timeout()函数稍微修改此行为。）
*/
int sqlite_exec(
        sqlite*,                      /* An open database */
                                      /* 一个打开的数据库 */
        const char *sql,              /* SQL to be executed */
                                      /* 要执行的SQL */
        sqlite_callback,              /* Callback function */
                                      /* 回调函数 */
        void *,                       /* 1st argument to callback function */
                                      /* 回调函数的第1个参数 */
        char **errmsg                 /* Error msg written here */
                                      /* 错误消息写在这里 */
);

/*
** Return values for sqlite_exec()
**
** sqlite_exec()的返回值
*/
#define SQLITE_OK           0   /* Successful result */
                                /* 成功结果 */
#define SQLITE_ERROR        1   /* SQL error or missing database */
                                /* SQL错误或缺少数据库 */
#define SQLITE_INTERNAL     2   /* An internal logic error in SQLite */
                                /* SQLite中的内部逻辑错误 */
#define SQLITE_PERM         3   /* Access permission denied */
                                /* 访问权限被拒绝 */
#define SQLITE_ABORT        4   /* Callback routine requested an abort */
                                /* 回调例程请求中止 */
#define SQLITE_BUSY         5   /* The database file is locked */
                                /* 数据库文件被锁定 */
#define SQLITE_LOCKED       6   /* A table in the database is locked */
                                /* 数据库中的表被锁定 */
#define SQLITE_NOMEM        7   /* A malloc() failed */
                                /* malloc()失败 */
#define SQLITE_READONLY     8   /* Attempt to write a readonly database */
                                /* 尝试写入只读数据库 */
#define SQLITE_INTERRUPT    9   /* Operation terminated by sqlite_interrupt() */
                                /* 操作由sqlite_interrupt()终止 */
#define SQLITE_IOERR       10   /* Some kind of disk I/O error occurred */
                                /* 发生某种磁盘I/O错误 */
#define SQLITE_CORRUPT     11   /* The database disk image is malformed */
                                /* 数据库磁盘映像格式错误 */
#define SQLITE_NOTFOUND    12   /* (Internal Only) Table or record not found */
                                /* （仅限内部）未找到表或记录 */
#define SQLITE_FULL        13   /* Insertion failed because database is full */
                                /* 插入失败，因为数据库已满 */
#define SQLITE_CANTOPEN    14   /* Unable to open the database file */
                                /* 无法打开数据库文件 */
#define SQLITE_PROTOCOL    15   /* Database lock protocol error */
                                /* 数据库锁定协议错误 */
#define SQLITE_EMPTY       16   /* (Internal Only) Database table is empty */
                                /* （仅限内部）数据库表为空 */
#define SQLITE_SCHEMA      17   /* The database schema changed */
                                /* 数据库架构已更改 */
#define SQLITE_TOOBIG      18   /* Too much data for one row of a table */
                                /* 表的一行数据过多 */
#define SQLITE_CONSTRAINT  19   /* Abort due to contraint violation */
                                /* 由于违反约束而中止 */
#define SQLITE_MISMATCH    20   /* Data type mismatch */
                                /* 数据类型不匹配 */
#define SQLITE_MISUSE      21   /* Library used incorrectly */
                                /* 库使用不正确 */

/*
** Each entry in an SQLite table has a unique integer key.  (The key is
** the value of the INTEGER PRIMARY KEY column if there is such a column,
** otherwise the key is generated at random.  The unique key is always
** available as the ROWID, OID, or _ROWID_ column.)  The following routine
** returns the integer key of the most recent insert in the database.
**
** This function is similar to the mysql_insert_id() function from MySQL.
**
** SQLite表中的每个条目都有一个唯一的整数键。（如果有INTEGER PRIMARY KEY列，
** 则键是该列的值，否则键是随机生成的。唯一键始终可用作ROWID、OID或_ROWID_列。）
** 以下例程返回数据库中最近插入的整数键。
**
** 此函数类似于MySQL中的mysql_insert_id()函数。
*/
int sqlite_last_insert_rowid(sqlite*);

/*
** This function returns the number of database rows that were changed
** (or inserted or deleted) by the most recent called sqlite_exec().
**
** All changes are counted, even if they were later undone by a
** ROLLBACK or ABORT.  Except, changes associated with creating and
** dropping tables are not counted.
**
** If a callback invokes sqlite_exec() recursively, then the changes
** in the inner, recursive call are counted together with the changes
** in the outer call.
**
** SQLite implements the command "DELETE FROM table" without a WHERE clause
** by dropping and recreating the table.  (This is much faster than going
** through and deleting individual elements form the table.)  Because of
** this optimization, the change count for "DELETE FROM table" will be
** zero regardless of the number of elements that were originally in the
** table. To get an accurate count of the number of rows deleted, use
** "DELETE FROM table WHERE 1" instead.
**
** 此函数返回最近调用的sqlite_exec()更改（或插入或删除）的数据库行数。
**
** 计算所有更改，即使它们后来被ROLLBACK或ABORT撤消。
** 但是，与创建和删除表相关的更改不计算在内。
**
** 如果回调递归调用sqlite_exec()，则内部递归调用中的更改将与外部调用中的更改一起计算。
**
** SQLite通过删除并重新创建表来实现没有WHERE子句的命令“DELETE FROM table”。
** （这比遍历并删除表中的单个元素要快得多。）由于此优化，
** 无论表中最初有多少元素，“DELETE FROM table”的更改计数都将为零。
** 要获得删除行数的准确计数，请改用“DELETE FROM table WHERE 1”。
*/
int sqlite_changes(sqlite*);

/* If the parameter to this routine is one of the return value constants
** defined above, then this routine returns a constant text string which
** descripts (in English) the meaning of the return value.
**
** 如果此例程的参数是上面定义的返回值常量之一，
** 则此例程返回一个常量文本字符串，该字符串（用英语）描述返回值的含义。
*/
const char *sqlite_error_string(int);
#define sqliteErrStr sqlite_error_string  /* Legacy. Do not use in new code. */
                                          /* 遗留。不要在新代码中使用。 */

/* This function causes any pending database operation to abort and
** return at its earliest opportunity.  This routine is typically
** called in response to a user action such as pressing "Cancel"
** or Ctrl-C where the user wants a long query operation to halt
** immediately.
**
** 此函数导致任何挂起的数据库操作中止并在最早的机会返回。
** 此例程通常是响应用户操作（例如按“取消”或Ctrl-C）而调用的，
** 其中用户希望立即停止长查询操作。
*/
void sqlite_interrupt(sqlite*);


/* This function returns true if the given input string comprises
** one or more complete SQL statements.
**
** The algorithm is simple.  If the last token other than spaces
** and comments is a semicolon, then return true.  otherwise return
** false.
**
** 如果给定的输入字符串包含一个或多个完整的SQL语句，则此函数返回true。
**
** 算法很简单。如果除空格和注释之外的最后一个标记是分号，则返回true。否则返回false。
*/
int sqlite_complete(const char *sql);

/*
** This routine identifies a callback function that is invoked
** whenever an attempt is made to open a database table that is
** currently locked by another process or thread.  If the busy callback
** is NULL, then sqlite_exec() returns SQLITE_BUSY immediately if
** it finds a locked table.  If the busy callback is not NULL, then
** sqlite_exec() invokes the callback with three arguments.  The
** second argument is the name of the locked table and the third
** argument is the number of times the table has been busy.  If the
** busy callback returns 0, then sqlite_exec() immediately returns
** SQLITE_BUSY.  If the callback returns non-zero, then sqlite_exec()
** tries to open the table again and the cycle repeats.
**
** The default busy callback is NULL.
**
** Sqlite is re-entrant, so the busy handler may start a new query.
** (It is not clear why anyone would every want to do this, but it
** is allowed, in theory.)  But the busy handler may not close the
** database.  Closing the database from a busy handler will delete
** data structures out from under the executing query and will
** probably result in a coredump.
**
** 此例程标识一个回调函数，每当尝试打开当前被另一个进程或线程锁定的数据库表时，
** 都会调用该函数。如果忙碌回调为NULL，则sqlite_exec()如果发现锁定的表，
** 将立即返回SQLITE_BUSY。如果忙碌回调不为NULL，则sqlite_exec()使用三个参数调用回调。
** 第二个参数是锁定表的名称，第三个参数是表忙碌的次数。
** 如果忙碌回调返回0，则sqlite_exec()立即返回SQLITE_BUSY。
** 如果回调返回非零值，则sqlite_exec()尝试再次打开表，并重复该循环。
**
** 默认的忙碌回调是NULL。
**
** Sqlite是可重入的，因此忙碌处理程序可能会启动新的查询。
** （目前尚不清楚为什么有人会想要这样做，但在理论上是允许的。）
** 但是忙碌处理程序不能关闭数据库。从忙碌处理程序关闭数据库将删除正在执行的查询下的数据结构，
** 并可能导致核心转储。
*/
void sqlite_busy_handler(sqlite*, int(*)(void*,const char*,int), void*);

/*
** This routine sets a busy handler that sleeps for a while when a
** table is locked.  The handler will sleep multiple times until
** at least "ms" milleseconds of sleeping have been done.  After
** "ms" milleseconds of sleeping, the handler returns 0 which
** causes sqlite_exec() to return SQLITE_BUSY.
**
** Calling this routine with an argument less than or equal to zero
** turns off all busy handlers.
**
** 此例程设置一个忙碌处理程序，当表被锁定时，该处理程序会休眠一段时间。
** 处理程序将多次休眠，直到至少完成了“ms”毫秒的休眠。
** 休眠“ms”毫秒后，处理程序返回0，这将导致sqlite_exec()返回SQLITE_BUSY。
**
** 使用小于或等于零的参数调用此例程将关闭所有忙碌处理程序。
*/
void sqlite_busy_timeout(sqlite*, int ms);

/*
** This next routine is really just a wrapper around sqlite_exec().
** Instead of invoking a user-supplied callback for each row of the
** result, this routine remembers each row of the result in memory
** obtained from malloc(), then returns all of the result after the
** query has finished.
**
** As an example, suppose the query result where this table:
**
**        Name        | Age
**        -----------------------
**        Alice       | 43
**        Bob         | 28
**        Cindy       | 21
**
** If the 3rd argument were &azResult then after the function returns
** azResult will contain the following data:
**
**        azResult[0] = "Name";
**        azResult[1] = "Age";
**        azResult[2] = "Alice";
**        azResult[3] = "43";
**        azResult[4] = "Bob";
**        azResult[5] = "28";
**        azResult[6] = "Cindy";
**        azResult[7] = "21";
**
** Notice that there is an extra row of data containing the column
** headers.  But the *nrow return value is still 3.  *ncolumn is
** set to 2.  In general, the number of values inserted into azResult
** will be ((*nrow) + 1)*(*ncolumn).
**
** After the calling function has finished using the result, it should
** pass the result data pointer to sqlite_free_table() in order to
** release the memory that was malloc-ed.  Because of the way the
** malloc() happens, the calling function must not try to call
** malloc() directly.  Only sqlite_free_table() is able to release
** the memory properly and safely.
**
** The return value of this routine is the same as from sqlite_exec().
**
** 下一个例程实际上只是sqlite_exec()的包装器。
** 此例程不是为结果的每一行调用用户提供的回调，
** 而是将结果的每一行记住在从malloc()获得的内存中，
** 然后在查询完成后返回所有结果。
**
** 作为一个例子，假设查询结果是这个表：
**
**        Name        | Age
**        -----------------------
**        Alice       | 43
**        Bob         | 28
**        Cindy       | 21
**
** 如果第3个参数是&azResult，那么在函数返回后，azResult将包含以下数据：
**
**        azResult[0] = "Name";
**        azResult[1] = "Age";
**        azResult[2] = "Alice";
**        azResult[3] = "43";
**        azResult[4] = "Bob";
**        azResult[5] = "28";
**        azResult[6] = "Cindy";
**        azResult[7] = "21";
**
** 请注意，有一行额外的数据包含列标题。但是*nrow返回值仍然是3。
** *ncolumn设置为2。通常，插入到azResult中的值的数量将是((*nrow) + 1)*(*ncolumn)。
**
** 调用函数使用完结果后，应将结果数据指针传递给sqlite_free_table()，
** 以释放malloc分配的内存。由于malloc()发生的方式，调用函数不得尝试直接调用malloc()。
** 只有sqlite_free_table()才能正确安全地释放内存。
**
** 此例程的返回值与sqlite_exec()相同。
*/
int sqlite_get_table(
        sqlite*,               /* An open database */
                               /* 一个打开的数据库 */
        const char *sql,       /* SQL to be executed */
                               /* 要执行的SQL */
        char ***resultp,       /* Result written to a char *[]  that this points to */
                               /* 结果写入此指向的 char *[] */
        int *nrow,             /* Number of result rows written here */
                               /* 结果行数写在这里 */
        int *ncolumn,          /* Number of result columns written here */
                               /* 结果列数写在这里 */
        char **errmsg          /* Error msg written here */
                               /* 错误消息写在这里 */
);

/*
** Call this routine to free the memory that sqlite_get_table() allocated.
**
** 调用此例程以释放sqlite_get_table()分配的内存。
*/
void sqlite_free_table(char **result);

/*
** The following routines are wrappers around sqlite_exec() and
** sqlite_get_table().  The only difference between the routines that
** follow and the originals is that the second argument to the
** routines that follow is really a printf()-style format
** string describing the SQL to be executed.  Arguments to the format
** string appear at the end of the argument list.
**
** All of the usual printf formatting options apply.  In addition, there
** is a "%q" option.  %q works like %s in that it substitutes a null-terminated
** string from the argument list.  But %q also doubles every '\'' character.
** %q is designed for use inside a string literal.  By doubling each '\''
** character it escapes that character and allows it to be inserted into
** the string.
**
** For example, so some string variable contains text as follows:
**
**      char *zText = "It's a happy day!";
**
** We can use this text in an SQL statement as follows:
**
**      sqlite_exec_printf(db, "INSERT INTO table VALUES('%q')",
**          callback1, 0, 0, zText);
**
** Because the %q format string is used, the '\'' character in zText
** is escaped and the SQL generated is as follows:
**
**      INSERT INTO table1 VALUES('It''s a happy day!')
**
** This is correct.  Had we used %s instead of %q, the generated SQL
** would have looked like this:
**
**      INSERT INTO table1 VALUES('It's a happy day!');
**
** This second example is an SQL syntax error.  As a general rule you
** should always use %q instead of %s when inserting text into a string
** literal.
**
** 以下例程是sqlite_exec()和sqlite_get_table()的包装器。
** 随后的例程与原始例程的唯一区别在于，随后的例程的第二个参数
** 实际上是一个printf()风格的格式字符串，描述了要执行的SQL。
** 格式字符串的参数出现在参数列表的末尾。
**
** 所有常用的printf格式选项都适用。此外，还有一个“%q”选项。
** %q的工作方式类似于%s，因为它从参数列表中替换一个以null结尾的字符串。
** 但是%q还会双写每个'\''字符。%q设计用于字符串字面量内部。
** 通过双写每个'\''字符，它转义该字符并允许将其插入字符串中。
**
** 例如，某个字符串变量包含如下文本：
**
**      char *zText = "It's a happy day!";
**
** 我们可以如下在SQL语句中使用此文本：
**
**      sqlite_exec_printf(db, "INSERT INTO table VALUES('%q')",
**          callback1, 0, 0, zText);
**
** 因为使用了%q格式字符串，zText中的'\''字符被转义，生成的SQL如下：
**
**      INSERT INTO table1 VALUES('It''s a happy day!')
**
** 这是正确的。如果我们使用%s而不是%q，生成的SQL将如下所示：
**
**      INSERT INTO table1 VALUES('It's a happy day!');
**
** 第二个示例是一个SQL语法错误。作为一般规则，
** 当将文本插入字符串字面量时，应始终使用%q而不是%s。
*/
int sqlite_exec_printf(
        sqlite*,                      /* An open database */
                                      /* 一个打开的数据库 */
        const char *sqlFormat,        /* printf-style format string for the SQL */
                                      /* SQL的printf风格格式字符串 */
        sqlite_callback,              /* Callback function */
                                      /* 回调函数 */
        void *,                       /* 1st argument to callback function */
                                      /* 回调函数的第1个参数 */
        char **errmsg,                /* Error msg written here */
                                      /* 错误消息写在这里 */
        ...                           /* Arguments to the format string. */
                                      /* 格式字符串的参数。 */
);
int sqlite_exec_vprintf(
        sqlite*,                      /* An open database */
                                      /* 一个打开的数据库 */
        const char *sqlFormat,        /* printf-style format string for the SQL */
                                      /* SQL的printf风格格式字符串 */
        sqlite_callback,              /* Callback function */
                                      /* 回调函数 */
        void *,                       /* 1st argument to callback function */
                                      /* 回调函数的第1个参数 */
        char **errmsg,                /* Error msg written here */
                                      /* 错误消息写在这里 */
        va_list ap                    /* Arguments to the format string. */
                                      /* 格式字符串的参数。 */
);
int sqlite_get_table_printf(
        sqlite*,               /* An open database */
                               /* 一个打开的数据库 */
        const char *sqlFormat, /* printf-style format string for the SQL */
                               /* SQL的printf风格格式字符串 */
        char ***resultp,       /* Result written to a char *[]  that this points to */
                               /* 结果写入此指向的 char *[] */
        int *nrow,             /* Number of result rows written here */
                               /* 结果行数写在这里 */
        int *ncolumn,          /* Number of result columns written here */
                               /* 结果列数写在这里 */
        char **errmsg,         /* Error msg written here */
                               /* 错误消息写在这里 */
        ...                    /* Arguments to the format string */
                               /* 格式字符串的参数 */
);
int sqlite_get_table_vprintf(
        sqlite*,               /* An open database */
                               /* 一个打开的数据库 */
        const char *sqlFormat, /* printf-style format string for the SQL */
                               /* SQL的printf风格格式字符串 */
        char ***resultp,       /* Result written to a char *[]  that this points to */
                               /* 结果写入此指向的 char *[] */
        int *nrow,             /* Number of result rows written here */
                               /* 结果行数写在这里 */
        int *ncolumn,          /* Number of result columns written here */
                               /* 结果列数写在这里 */
        char **errmsg,         /* Error msg written here */
                               /* 错误消息写在这里 */
        va_list ap             /* Arguments to the format string */
                               /* 格式字符串的参数 */
);

/*
** Windows systems should call this routine to free memory that
** is returned in the in the errmsg parameter of sqlite_open() when
** SQLite is a DLL.  For some reason, it does not work to call free()
** directly.
**
** 当SQLite是DLL时，Windows系统应调用此例程以释放
** sqlite_open()的errmsg参数中返回的内存。
** 由于某种原因，直接调用free()不起作用。
*/
void sqlite_freemem(void *p);

/*
** Windows systems need functions to call to return the sqlite_version
** and sqlite_encoding strings.
**
** Windows系统需要调用函数来返回sqlite_version和sqlite_encoding字符串。
*/
const char *sqlite_libversion(void);
const char *sqlite_libencoding(void);

/*
** A pointer to the following structure is used to communicate with
** the implementations of user-defined functions.
**
** 指向以下结构的指针用于与用户定义函数的实现进行通信。
*/
typedef struct sqlite_func sqlite_func;

/*
** Use the following routines to create new user-defined functions.  See
** the documentation for details.
**
** 使用以下例程创建新的用户定义函数。有关详细信息，请参阅文档。
*/
int sqlite_create_function(
        sqlite*,                  /* Database where the new function is registered */
                                  /* 注册新函数的数据库 */
        const char *zName,        /* Name of the new function */
                                  /* 新函数的名称 */
        int nArg,                 /* Number of arguments.  -1 means any number */
                                  /* 参数数量。-1表示任意数量 */
        void (*xFunc)(sqlite_func*,int,const char**),  /* C code to implement */
                                                       /* 实现的C代码 */
        void *pUserData           /* Available via the sqlite_user_data() call */
                                  /* 可通过sqlite_user_data()调用获得 */
);
int sqlite_create_aggregate(
        sqlite*,                  /* Database where the new function is registered */
                                  /* 注册新函数的数据库 */
        const char *zName,        /* Name of the function */
                                  /* 函数的名称 */
        int nArg,                 /* Number of arguments */
                                  /* 参数数量 */
        void (*xStep)(sqlite_func*,int,const char**), /* Called for each row */
                                                      /* 为每一行调用 */
        void (*xFinalize)(sqlite_func*),       /* Called once to get final result */
                                               /* 调用一次以获得最终结果 */
        void *pUserData           /* Available via the sqlite_user_data() call */
                                  /* 可通过sqlite_user_data()调用获得 */
);

/*
** The user function implementations call one of the following four routines
** in order to return their results.  The first parameter to each of these
** routines is a copy of the first argument to xFunc() or xFinialize().
** The second parameter to these routines is the result to be returned.
** A NULL can be passed as the second parameter to sqlite_set_result_string()
** in order to return a NULL result.
**
** The 3rd argument to _string and _error is the number of characters to
** take from the string.  If this argument is negative, then all characters
** up to and including the first '\000' are used.
**
** The sqlite_set_result_string() function allocates a buffer to hold the
** result and returns a pointer to this buffer.  The calling routine
** (that is, the implmentation of a user function) can alter the content
** of this buffer if desired.
**
** 用户函数实现调用以下四个例程之一以返回其结果。
** 这些例程的第一个参数是xFunc()或xFinialize()的第一个参数的副本。
** 这些例程的第二个参数是要返回的结果。
** 可以将NULL作为第二个参数传递给sqlite_set_result_string()以返回NULL结果。
**
** _string和_error的第3个参数是从字符串中获取的字符数。
** 如果此参数为负数，则使用直到并包括第一个'\000'的所有字符。
**
** sqlite_set_result_string()函数分配一个缓冲区来保存结果，并返回指向此缓冲区的指针。
** 调用例程（即用户函数的实现）可以根据需要更改此缓冲区的内容。
*/
char *sqlite_set_result_string(sqlite_func*,const char*,int);
void sqlite_set_result_int(sqlite_func*,int);
void sqlite_set_result_double(sqlite_func*,double);
void sqlite_set_result_error(sqlite_func*,const char*,int);

/*
** The pUserData parameter to the sqlite_create_function() and
** sqlite_create_aggregate() routines used to register user functions
** is available to the implementation of the function using this
** call.
**
** 用于注册用户函数的sqlite_create_function()和sqlite_create_aggregate()例程的
** pUserData参数可供使用此调用的函数实现使用。
*/
void *sqlite_user_data(sqlite_func*);

/*
** Aggregate functions use the following routine to allocate
** a structure for storing their state.  The first time this routine
** is called for a particular aggregate, a new structure of size nBytes
** is allocated, zeroed, and returned.  On subsequent calls (for the
** same aggregate instance) the same buffer is returned.  The implementation
** of the aggregate can use the returned buffer to accumulate data.
**
** The buffer allocated is freed automatically be SQLite.
**
** 聚合函数使用以下例程分配用于存储其状态的结构。
** 第一次为特定聚合调用此例程时，将分配、归零并返回大小为nBytes的新结构。
** 在随后的调用（对于相同的聚合实例）中，返回相同的缓冲区。
** 聚合的实现可以使用返回的缓冲区来累积数据。
**
** 分配的缓冲区由SQLite自动释放。
*/
void *sqlite_aggregate_context(sqlite_func*, int nBytes);

/*
** The next routine returns the number of calls to xStep for a particular
** aggregate function instance.  The current call to xStep counts so this
** routine always returns at least 1.
**
** 下一个例程返回特定聚合函数实例对xStep的调用次数。
** 当前对xStep的调用也计算在内，因此此例程始终返回至少1。
*/
int sqlite_aggregate_count(sqlite_func*);

#ifdef __cplusplus
}  /* End of the 'extern "C"' block */
#endif

#endif /* _SQLITE_H_ */
