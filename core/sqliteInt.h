
#ifndef UINT32_TYPE
# define UINT32_TYPE unsigned int
#endif
#ifndef UINT16_TYPE
# define UINT16_TYPE unsigned short int
#endif
#ifndef UINT8_TYPE
# define UINT8_TYPE unsigned char
#endif
#ifndef INTPTR_TYPE
# define INTPTR_TYPE int
#endif
typedef UINT32_TYPE u32;           /* 4-byte unsigned integer */
                                   /* 4字节无符号整数 */
typedef UINT16_TYPE u16;           /* 2-byte unsigned integer */
                                   /* 2字节无符号整数 */
typedef UINT8_TYPE u8;             /* 1-byte unsigned integer */
                                   /* 1字节无符号整数 */
typedef INTPTR_TYPE ptr;           /* Big enough to hold a pointer */
                                   /* 足够大以容纳指针 */
typedef unsigned INTPTR_TYPE uptr; /* Big enough to hold a pointer */
                                   /* 足够大以容纳指针 */
extern int sqlite_malloc_failed;
/*
** This macro casts a pointer to an integer.  Useful for doing
** pointer arithmetic.
**
** 此宏将指针转换为整数。用于进行指针运算。
*/
#define Addr(X)  ((uptr)X)
