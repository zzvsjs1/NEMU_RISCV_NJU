#include <stdio.h>
#include <assert.h>

int main()
{
    printf("Start!\n");
    /*
   * /share/files/num is a ramdisk-backed regular file used to exercise libc
   * seek/read/write paths above nanos-lite. The fixed 5000-byte size comes
   * from 1000 lines of four digits plus newline.
   */
    FILE *fp = fopen("/share/files/num", "r+");
    assert(fp);

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    assert(size == 5000);

    fseek(fp, 500 * 5, SEEK_SET);
    int i, n;
    /*
   * Seek to the middle, then write the first half and read both halves again.
   * This catches offset accounting bugs where writes accidentally disturb the
   * current file position or the untouched second half of the ramdisk file.
   */
    for (i = 500; i < 1000; i++)
    {
        fscanf(fp, "%d", &n);
        assert(n == i + 1);
    }

    fseek(fp, 0, SEEK_SET);
    for (i = 0; i < 500; i++)
    {
        fprintf(fp, "%4d\n", i + 1 + 1000);
    }

    for (i = 500; i < 1000; i++)
    {
        fscanf(fp, "%d", &n);
        assert(n == i + 1);
    }

    fseek(fp, 0, SEEK_SET);
    for (i = 0; i < 500; i++)
    {
        fscanf(fp, "%d", &n);
        assert(n == i + 1 + 1000);
    }

    fclose(fp);

    printf("PASS!!!\n");

    return 0;
}
