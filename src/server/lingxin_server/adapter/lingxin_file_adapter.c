#include "lingxin_file.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

bool lingxin_file_create(const char *file_path, int length)
{
    if (!file_path || length < 0) {
        return false;
    }

    FILE *fp = fopen(file_path, "wb");
    if (!fp) {
        return false;
    }

    if (length > 0) {
        if (fseek(fp, length - 1, SEEK_SET) != 0 || fputc('\0', fp) == EOF) {
            fclose(fp);
            return false;
        }
    }

    fclose(fp);
    return true;
}

bool lingxin_file_read(const char *file_path, char *buffer, int offet, int length)
{
    if (!file_path || !buffer || offet < 0 || length < 0) {
        return false;
    }

    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        return false;
    }

    bool ok = fseek(fp, offet, SEEK_SET) == 0 && fread(buffer, 1, length, fp) == (size_t)length;
    fclose(fp);
    return ok;
}

bool lingxin_file_write(const char *file_path, bool is_append, const char *data, int length)
{
    if (!file_path || !data || length < 0) {
        return false;
    }

    FILE *fp = fopen(file_path, is_append ? "ab" : "wb");
    if (!fp) {
        return false;
    }

    bool ok = fwrite(data, 1, length, fp) == (size_t)length;
    fclose(fp);
    return ok;
}

bool lingxin_file_exist(const char *file_path)
{
    struct stat st;
    return file_path && stat(file_path, &st) == 0;
}

int lingxin_file_length(const char *file_path)
{
    struct stat st;
    if (!file_path || stat(file_path, &st) != 0) {
        return -1;
    }
    return (int)st.st_size;
}

bool lingxin_file_delete(const char *file_path)
{
    return file_path && remove(file_path) == 0;
}

bool lingxin_file_clear(const char *file_path)
{
    if (!file_path) {
        return false;
    }

    FILE *fp = fopen(file_path, "wb");
    if (!fp) {
        return false;
    }
    fclose(fp);
    return true;
}
