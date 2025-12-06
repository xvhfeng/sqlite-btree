#include "../core/os.h"
#include "vfstest.h"
#include <stdio.h>
#include <assert.h>

/**
 * Test VFS implimetation by writing and reading back data into a file specified by fileLocation.
 * Step 1 : Open a new file
 * Step 2 : Seek for the offset 0 and write "hello this is vfs test"
 * Step 3 : Seek for the offset 6 and read 4 bytes
 * Step 4 : Overwrite offset 11 with ""is override""
 * Step 5 : Close the connection and delete files
 *
 * @param fileLocation
 *
 * 通过将数据写入并回读到由fileLocation指定的文件中来测试VFS实现。
 * 步骤 1：打开一个新文件
 * 步骤 2：寻找偏移量0并写入“hello this is vfs test”
 * 步骤 3：寻找偏移量6并读取4个字节
 * 步骤 4：用“is override”覆盖偏移量11
 * 步骤 5：关闭连接并删除文件
 *
 * @param fileLocation
 */
void testVFS(char **fileLocation) {
    printf("\nStart running VFS tests\n");
    OsFile fd;
    int readOnly = 0;

    /** Step 1 **/
    /** 步骤 1 **/
    printf("Open file from %s\n", *fileLocation);
    int rc = sqliteOsOpenReadWrite(*fileLocation, &fd, &readOnly);
    printf("File open status %d\n" ,rc);
    int fileExist = sqliteOsFileExists(*fileLocation);
    printf("Does file exist %d\n", fileExist);

    // File should be exist when open a file.
    // 打开文件时文件应该存在。
    assert(fileExist == 1);

    /** Step 2 **/
    /** 步骤 2 **/
    // Seek offset 0 on the database fle
    // 在数据库文件上寻找偏移量0
    sqliteOsSeek(&fd, 0);
    char *valueToWrite = "hello this is vfs test";
    // write string to start offset 0 with the length of 22 byte
    // 将字符串写入起始偏移量0，长度为22字节
    sqliteOsWrite(&fd, valueToWrite, 22);

    /** step 3 **/
    /** 步骤 3 **/
    // Seek offset 6 on the database file
    // 在数据库文件上寻找偏移量6
    sqliteOsSeek(&fd, 6);
    char readBuffer[16];
    // Read 4 bytes from offset 6 and write it into readBuffer
    // 从偏移量6读取4个字节并将其写入readBuffer
    sqliteOsRead(&fd, &readBuffer, 4);
    printf("Read from offset: %s\n", readBuffer);

    // Extracted data should be "this"
    // 提取的数据应该是“this”
    assert(strncmp(readBuffer, "this", 4) == 0);

    /** Step 4 **/
    /** 步骤 4 **/
    // Seek offset 11 on database file
    // 在数据库文件上寻找偏移量11
    sqliteOsSeek(&fd, 11);
    char *valueToOverride = "is override";
    // Write string to offset 11
    // 将字符串写入偏移量11
    sqliteOsWrite(&fd, valueToOverride,11);

    // Seek offset 0 on database file
    // 在数据库文件上寻找偏移量0
    sqliteOsSeek(&fd, 0);
    char finalResult[22];
    // Read the string from offset 0 and write the result into finalResult
    // 从偏移量0读取字符串并将结果写入finalResult
    sqliteOsRead(&fd, &finalResult,22);
    printf("Final result: %s\n", finalResult);

    // After override the initial string
    // 覆盖初始字符串后
    assert(strncmp(finalResult, "hello this is override", 22) == 0);

    /** Step 5 **/
    /** 步骤 5 **/
    // Close the file connection
    // 关闭文件连接
    rc = sqliteOsClose(&fd);
    printf("File close status: %d\n", rc);

    // Delete the file
    // 删除文件
    sqliteOsDelete(*fileLocation);

    // Check if file exist
    // 检查文件是否存在
    fileExist = sqliteOsFileExists(*fileLocation);
    printf("Does file exist after delete %d\n", fileExist);

    assert(fileExist == 0);

}
