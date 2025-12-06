#include "pager.h"
#include "os.h"
#include <stdio.h>
#include <assert.h>

/**
 * Test Pager implemetation by reading and writing into a page
 * Step 1 : Open new file and create three pages
 * Step 2 : Write data into three different pages and save it on the file
 * Step 3 : Read pages to make sure changes commited
 * Step 4 : Write data into the third page and before commit the changes, rollback to the previous state
 *
 * @param fileLocation
 *
 * 通过读写页面测试分页器实现
 * 步骤 1：打开新文件并创建三个页面
 * 步骤 2：将数据写入三个不同的页面并将其保存在文件上
 * 步骤 3：读取页面以确保更改已提交
 * 步骤 4：将数据写入第三个页面，在提交更改之前，回滚到先前的状态
 *
 * @param fileLocation
 */
void testPager(char **fileLocation) {
    printf("\nStart running Pager tests\n");
    char zBuf[100];
    int rc;
    Pager *pPager;
    void *pPage;

    /** Step 1 : Open new file and create three pages **/
    /** 步骤 1：打开新文件并创建三个页面 **/
    // Open new database file in given location with 10 max in memory cache and 0 extra byte append to each
    // in memory page
    // 在给定位置打开新的数据库文件，内存缓存最大为10，每个内存页面附加0个额外字节
    rc = sqlitepager_open(&pPager, *fileLocation, 10, 0);

    // Create page with given page numbers
    // page numbers should be starts from one
    // 使用给定的页码创建页面
    // 页码应从1开始
    rc = sqlitepager_get(pPager, 1, &pPage);
    rc = sqlitepager_get(pPager, 2, &pPage);
    rc = sqlitepager_get(pPager, 3, &pPage);


    /** Step 2 : Write data into three different pages and save it on the file **/
    /** 步骤 2：将数据写入三个不同的页面并将其保存在文件上 **/
    // Look for the first page
    // 查找第一页
    pPage = sqlitepager_lookup(pPager, 1);
    // Start write page
    // 开始写入页面
    rc = sqlitepager_write(pPage);
    // Add string into the page
    // 将字符串添加到页面中
    strncpy((char*)pPage, "Page One", SQLITE_PAGE_SIZE-1);
    // Commit page data into the file
    // 将页面数据提交到文件中
    rc = sqlitepager_commit(pPager);

    pPage = sqlitepager_lookup(pPager, 2);
    rc = sqlitepager_write(pPage);
    strncpy((char*)pPage, "Page Two", SQLITE_PAGE_SIZE-1);
    rc = sqlitepager_commit(pPager);

    pPage = sqlitepager_lookup(pPager, 3);
    rc = sqlitepager_write(pPage);
    strncpy((char*)pPage, "Page Three", SQLITE_PAGE_SIZE-1);
    rc = sqlitepager_commit(pPager);

    /** Step 3 : Read pages to make sure changes commited **/
    /** 步骤 3：读取页面以确保更改已提交 **/
    sqlitepager_get(pPager, 1, &pPage);
    memcpy(zBuf, pPage, sizeof(zBuf));
    printf("Read page result1: %s\n", zBuf);
    assert(strncmp(zBuf, "Page One", 8) == 0);

    sqlitepager_get(pPager, 2, &pPage);
    memcpy(zBuf, pPage, sizeof(zBuf));
    printf("Read page result2: %s\n", zBuf);
    assert(strncmp(zBuf, "Page Two", 8) == 0);

    sqlitepager_get(pPager, 3, &pPage);
    memcpy(zBuf, pPage, sizeof(zBuf));
    printf("Read page result3: %s\n", zBuf);
    assert(strncmp(zBuf, "Page Three", 10) == 0);

    /** Step 4 : Write data into the third page and before commit the changes, rollback to the previous state **/
    /** 步骤 4：将数据写入第三个页面，在提交更改之前，回滚到先前的状态 **/
    pPage = sqlitepager_lookup(pPager, 3);
    rc = sqlitepager_write(pPage);
    strncpy((char*)pPage, "Page test rollback", SQLITE_PAGE_SIZE-1);
    // Rallback changes to the previous state
    // 将更改回滚到先前的状态
    sqlitepager_rollback(pPager);
    rc = sqlitepager_commit(pPager);

    sqlitepager_get(pPager, 3, &pPage);
    memcpy(zBuf, pPage, sizeof(zBuf));
    printf("Read page result4: %s\n", zBuf);
    assert(strncmp(zBuf, "Page Three", 10) == 0);


    printf("Page count: %d\n",sqlitepager_pagecount(pPager));
    assert(sqlitepager_pagecount(pPager) == 3);

    sqlitepager_close(pPager);
    sqliteOsDelete(*fileLocation);
}