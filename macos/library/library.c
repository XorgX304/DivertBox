#include "library.h"

#include <CommonCrypto/CommonDigest.h>
#include <kext/KextManager.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
#include <sys/kern_control.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <ftw.h>

// -------- superuser access

int library_superuser_access()
{
    return geteuid();
}

// -------- string to hexstring

int library_string_to_hexstring(const char* string, int string_length, char* hexstring, int hexstring_length)
{
    if (string_length * 2 != hexstring_length)
    {
        return 1;
    }

    for (int index = 0; index < string_length; ++index)
    {
        if (snprintf(hexstring, hexstring_length, "%02X", string[index]) < 0)
        {
            return -1;
        }

        hexstring += 2;
    }

    return 0;
}

// -------- directory exists

int library_directory_exists(const char* path)
{
    struct stat s;
    return stat(path, &s) || !S_ISDIR(s.st_mode);
}

// -------- directory md5sum

CC_MD5_CTX _library_md5_context;

int _library_md5sum_ftw_handle(const char* fpath, const struct stat* sb, int typeflag, struct FTW* ftwbuf)
{
    if (S_ISDIR(sb->st_mode))
    {
        return CC_MD5_Update(&_library_md5_context, fpath, strlen(fpath)) != 1;
    }

    if (S_ISREG(sb->st_mode))
    {
        FILE* file = fopen(fpath, "rb");
        if (file == NULL) { return 0; }

        char buffer[UINT8_MAX];
        size_t length = 0;

        do
        {
            length = fread(buffer, 1, UINT8_MAX, file);

            if (CC_MD5_Update(&_library_md5_context, buffer, length) != 1)
            {
                fclose(file);
                return 1;
            }
        }
        while (length > 0);

        fclose(file);
    }

    return 0;
}

int library_directory_md5sum(const char* path, unsigned char* checksum)
{
    return
        CC_MD5_Init(&_library_md5_context) != 1 ||
        CC_MD5_Update(&_library_md5_context, kBUNDLE_ID, strlen(kBUNDLE_ID)) != 1 ||
        nftw(path, &_library_md5sum_ftw_handle, 4, FTW_PHYS | FTW_DEPTH) ||
        CC_MD5_Final(checksum, &_library_md5_context) != 1;
}

// -------- directory chown

uid_t _library_chown_user_id = 0;
gid_t _library_chown_group_id = 0;

int _library_chown_ftw_handle(const char* fpath, const struct stat* sb, int typeflag, struct FTW* ftwbuf)
{
    if (S_ISDIR(sb->st_mode) || S_ISREG(sb->st_mode))
    {
        return chown(fpath, _library_chown_user_id, _library_chown_group_id);
    }

    return 0;
}

int library_directory_chown(const char* path, const char* user, const char* group)
{
    _library_chown_user_id = 0;
    {
        struct passwd* pwd = getpwnam(user);
        if (pwd == NULL) { return -1; }
        _library_chown_user_id = pwd->pw_uid;
    }

    _library_chown_group_id = 0;
    {
        struct group* grp = getgrnam(group);
        if (grp == NULL) { return -1; }
        _library_chown_group_id = grp->gr_gid;
    }

    return nftw(path, &_library_chown_ftw_handle, 4, FTW_PHYS | FTW_DEPTH);
}

// -------- kext load with directory

int library_kext_load_with_directory(const char* bundle_dir)
{
    CFStringRef path_string = CFStringCreateWithCString(kCFAllocatorDefault, bundle_dir, kCFStringEncodingUTF8);
    if (path_string == NULL) { return -1; }

    CFURLRef path_url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, path_string, kCFURLPOSIXPathStyle, true);
    if (path_url == NULL) { return -1; }

    return KextManagerLoadKextWithURL(path_url, NULL) != kOSReturnSuccess;
}

// -------- kext loaded and valid

typedef struct
{
    int result;
    const char* id;
    size_t id_length;
    const char* path;
    size_t path_length;
} _library_kext_check_context_t;

void _library_kext_check_enumerate(const void* key, const void* value, void* context)
{
    if (key == NULL || value == NULL || context == NULL) { return; }

    _library_kext_check_context_t* check_context = context;
    if (check_context->result != -1) { return; }

    const char* bundle_id_cstring = CFStringGetCStringPtr(key, kCFStringEncodingUTF8);
    if (bundle_id_cstring == NULL) { return; }

    const char* bundle_path_cstring = CFStringGetCStringPtr(CFDictionaryGetValue(value, CFStringCreateWithCString(kCFAllocatorDefault, "OSBundlePath", kCFStringEncodingUTF8)), kCFStringEncodingUTF8);
    if (bundle_path_cstring == NULL) { return; }

    if (strncmp(bundle_id_cstring, check_context->id, check_context->id_length) == 0)
    {
        check_context->result = strncmp(bundle_path_cstring, check_context->path, check_context->path_length) != 0;
    }

    if (strncmp(bundle_path_cstring, check_context->path, check_context->path_length) == 0)
    {
        check_context->result = strncmp(bundle_id_cstring, check_context->id, check_context->id_length) != 0;
    }
}

int library_kext_loaded_and_valid(const char* bundle_id, const char* bundle_dir)
{
    CFDictionaryRef loaded_kexts = KextManagerCopyLoadedKextInfo(NULL, NULL);
    if (loaded_kexts == NULL) { return -1; }

    _library_kext_check_context_t context;

    context.result = -1;

    context.id = bundle_id;
    context.id_length = strlen(bundle_id);

    char actualpath[PATH_MAX];
    bzero(actualpath, PATH_MAX);
    if (realpath(bundle_dir, actualpath) == NULL) { return -1; }
    context.path = actualpath;
    context.path_length = strlen(actualpath);

    CFDictionaryApplyFunction(loaded_kexts, _library_kext_check_enumerate, &context);

    return context.result;
}

// -------- kext unload with id

int library_kext_unload_with_id(const char* bundle_id)
{
    CFStringRef bundle_id_string = CFStringCreateWithCString(kCFAllocatorDefault, bundle_id, kCFStringEncodingUTF8);
    if (bundle_id_string == NULL) { return -1; }

    return KextManagerUnloadKextWithIdentifier(bundle_id_string) != kOSReturnSuccess;
}

// -------- kext unload with directory

typedef struct
{
    int result;
    const char* path;
    size_t path_length;
} _library_kext_unload_context_t;

void _library_kext_unload_enumerate(const void* key, const void* value, void* context)
{
    if (key == NULL || value == NULL || context == NULL) { return; }

    _library_kext_unload_context_t* unload_context = context;
    if (unload_context->result != -1) { return; }

    const char* bundle_id_cstring = CFStringGetCStringPtr(key, kCFStringEncodingUTF8);
    if (bundle_id_cstring == NULL) { return; }

    const char* bundle_path_cstring = CFStringGetCStringPtr(CFDictionaryGetValue(value, CFStringCreateWithCString(kCFAllocatorDefault, "OSBundlePath", kCFStringEncodingUTF8)), kCFStringEncodingUTF8);
    if (bundle_path_cstring == NULL) { return; }

    if (strncmp(bundle_path_cstring, unload_context->path, unload_context->path_length) == 0)
    {
        unload_context->result = library_kext_unload_with_id(bundle_id_cstring);
    }
}

int library_kext_unload_with_directory(const char* bundle_dir)
{
    CFDictionaryRef loaded_kexts = KextManagerCopyLoadedKextInfo(NULL, NULL);
    if (loaded_kexts == NULL) { return -1; }

    _library_kext_unload_context_t context;

    context.result = -1;

    char actualpath[PATH_MAX];
    bzero(actualpath, PATH_MAX);
    if (realpath(bundle_dir, actualpath) == NULL) { return -1; }
    context.path = actualpath;
    context.path_length = strlen(actualpath);

    CFDictionaryApplyFunction(loaded_kexts, _library_kext_unload_enumerate, &context);

    return context.result;
}

// -------- control socket connect

int _library_socket = -1;

int library_control_socket_connect(const char* bundle_id)
{
    if (_library_socket > -1) { return -1; }

    _library_socket = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (_library_socket < 0) { return -1; }

    struct ctl_info info;
    bzero(&info, sizeof(info));
    strncpy(info.ctl_name, bundle_id, strlen(bundle_id));
    if (ioctl(_library_socket, CTLIOCGINFO, &info))
    {
        close(_library_socket);
        _library_socket = -1;

        return 1;
    }

    struct sockaddr_ctl addr;
    bzero(&addr, sizeof(addr));
    addr.sc_id = info.ctl_id;
    addr.sc_unit = 0;
    addr.sc_len = sizeof(addr);
    addr.sc_family = AF_SYSTEM;
    addr.ss_sysaddr = AF_SYS_CONTROL;
    if (connect(_library_socket, (struct sockaddr*)&addr, sizeof(addr)))
    {
        close(_library_socket);
        _library_socket = -1;

        return 2;
    }

    return 0;
}

// -------- control socket disconnect

int library_control_socket_disconnect()
{
    if (_library_socket < 0) { return 0; }

    int result = close(_library_socket);
    if (result == 0) { _library_socket = -1; }

    return result;
}

// -------- control socket option set

int library_control_socket_option_set(int option, const void* value, unsigned int length)
{
    return setsockopt(_library_socket, SYSPROTO_CONTROL, option, value, length);
}

// -------- control socket option get

int library_control_socket_option_get(int option, void* value, unsigned int* length)
{
    return getsockopt(_library_socket, SYSPROTO_CONTROL, option, value, length);
}

// -------- control socket receive

int library_control_socket_receive(void* buffer, unsigned int buffer_length, unsigned int* receive_length)
{
    int result = recv(_library_socket, buffer, buffer_length, MSG_WAITALL);
    if (result < 0) { return result; }

    *receive_length = result;
    return 0;
}

// -------- control socket send

int library_control_socket_send(const void* buffer, unsigned int buffer_length)
{
    int result = send(_library_socket, buffer, buffer_length, MSG_WAITALL);
    return result < 0 ? result : 0;
}
