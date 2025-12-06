
#include "core/os.h"
#include "test/vfstest.h"
#include "test/pagetest.h"
#include "test/btreetest.h"

int main() {
    /*
     * Since file location is different for Unix and Windows, Here fileLocation set according to the OS
     *
     * 由于Unix和Windows的文件位置不同，此处根据操作系统设置fileLocation
     */
    #if OS_UNIX
    char *fileLocation = "/tmp/test.db";
    #endif
    #if OS_WIN
    char *fileLocation = "C://test.db";
    #endif

    /* Tests for VFS functionality
    ** VFS功能测试
    */
    testVFS(&fileLocation);

    /* Tests for Pager functionality
    ** Pager功能测试
    */
    testPager(&fileLocation);

    /* Tests for Btree functionality
    ** Btree功能测试
    */
    testBtree(&fileLocation);
    return 0;
}