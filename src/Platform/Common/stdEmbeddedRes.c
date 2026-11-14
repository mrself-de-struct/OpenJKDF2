#include "stdEmbeddedRes.h"

#include "globals.h"
#include "stdPlatform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FS_POSIX
#include "external/fcaseopen/fcaseopen.h"
#endif

#ifdef SDL2_RENDER
#include "SDL2_helper.h"
#endif
//#define stdEmbeddedRes_errmsg(_msg) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", _msg, NULL)
//#else
#define stdEmbeddedRes_errmsg(_msg) stdPlatform_Printf("stdEmbeddedRes: %s\n", _msg)
//#endif

#ifdef TARGET_TWL
#include <nds.h>
#endif

#ifdef TARGET_RETRO_HOMEBREW
#include <sys/stat.h>
#endif

char* stdEmbeddedRes_LoadOnlyInternal(const char* filepath, size_t* pOutSz)
{
#ifdef TARGET_RETRO_HOMEBREW
    struct stat statstuff;
    int exists = 0;
#endif
    FILE* f = NULL;
    const char* base_path = NULL;
    char* file_contents = NULL;
    char tmp_filepath[256];
    strncpy(tmp_filepath, "resource/", 256-1);
    strncat(tmp_filepath, filepath, 256-1);

    if (pOutSz) {
        *pOutSz = 0;
    }

#ifdef WIN32
for (int i = 0; i < strlen(tmp_filepath); i++)
{
    if (tmp_filepath[i] == '/') {
        tmp_filepath[i] = '\\';
    }
}
#endif

#ifdef TARGET_RETRO_HOMEBREW
    exists = stat(tmp_filepath, &statstuff) >= 0;
    if (!exists) {
        goto skip_fopen;
    }
#endif

#if defined(MACOS) && defined(SDL2_RENDER)
    // SDL_GetBasePath() returns an internally-cached, non-owned string as of SDL3 -- must not SDL_free() it.
    base_path = SDL_GetBasePath();
    strncpy(tmp_filepath, base_path, 256-1);
    strncat(tmp_filepath, "Contents/Resources/", 256-1);
    strncat(tmp_filepath, filepath, 256-1);
#endif

    f = fopen(tmp_filepath, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        size_t len = ftell(f);
        rewind(f);
        
        file_contents = (char*)malloc(len+1);
        if (!file_contents) {
            if (pOutSz) {
                *pOutSz = 0;
            }
            fclose(f);
            return NULL;
        }
        
        if (fread(file_contents, 1, len, f) != len)
        {
            char errtmp[256];
            snprintf(errtmp, 256, "Failed to read file `%s`!\n", filepath);
            stdEmbeddedRes_errmsg(errtmp);
            return NULL;
        }
        file_contents[len] = 0;
        
        fclose(f);

        if (pOutSz) {
            *pOutSz = len+1;
        }
        return file_contents;
    }

skip_fopen:
    strncpy(tmp_filepath, filepath, 256-1);
    
    for (int i = 0; i < strlen(tmp_filepath); i++)
    {
        if (tmp_filepath[i] == '\\') {
            tmp_filepath[i] = '/';
        }
    }

    for (size_t i = 0; i < embeddedResource_aFiles_num; i++)
    {
        if (!strcmp(embeddedResource_aFiles[i].fpath, tmp_filepath)) {
            file_contents = (char*)malloc(embeddedResource_aFiles[i].data_len+1);
            if (!file_contents) {
                if (pOutSz) {
                    *pOutSz = 0;
                }
                return NULL;
            }
            memcpy(file_contents, embeddedResource_aFiles[i].data, embeddedResource_aFiles[i].data_len);
            file_contents[embeddedResource_aFiles[i].data_len] = 0;

            if (pOutSz) {
                *pOutSz = embeddedResource_aFiles[i].data_len+1;
            }

            break;
        }
    }

    return file_contents;
}

char* stdEmbeddedRes_Load(const char* filepath, size_t* pOutSz)
{
#ifdef TARGET_RETRO_HOMEBREW
    struct stat statstuff;
    int exists = 0;
#endif
    FILE* f = NULL;
    const char* base_path = NULL;
    char* file_contents = NULL;
    char tmp_filepath[256];
    strncpy(tmp_filepath, "resource/", 256-1);
    strncat(tmp_filepath, filepath, 256-1);

    if (pOutSz) {
        *pOutSz = 0;
    }

#ifdef WIN32
for (int i = 0; i < strlen(tmp_filepath); i++)
{
    if (tmp_filepath[i] == '/') {
        tmp_filepath[i] = '\\';
    }
}
#endif

#ifdef FS_POSIX
    char *r = (char*)malloc(strlen(tmp_filepath) + 16);
    if (casepath(tmp_filepath, r))
    {
        strcpy(tmp_filepath, r);
    }
    free(r);
#endif


#ifdef TARGET_RETRO_HOMEBREW
    exists = stat(tmp_filepath, &statstuff) >= 0;
    if (!exists) {
        goto skip_fopen;
    }
#endif

    f = fopen(tmp_filepath, "r");
    if (f)
    {
retry_file:
        fseek(f, 0, SEEK_END);
        size_t len = ftell(f);
        rewind(f);
        
        file_contents = (char*)malloc(len+1);
        if (!file_contents) {
            if (pOutSz) {
                *pOutSz = 0;
            }
            fclose(f);
            return NULL;
        }
        
        if (fread(file_contents, 1, len, f) != len)
        {
            char errtmp[256];
            snprintf(errtmp, 256, "Failed to read file `%s`!\n", filepath);
            stdEmbeddedRes_errmsg(errtmp);
            return NULL;
        }
        file_contents[len] = 0;
        
        fclose(f);

        if (pOutSz) {
            *pOutSz = len+1;
        }
    }
    else
    {
#if defined(MACOS) && defined(SDL2_RENDER)
        // SDL_GetBasePath() returns an internally-cached, non-owned string as of SDL3 -- must not SDL_free() it.
        base_path = SDL_GetBasePath();
        strncpy(tmp_filepath, base_path, 256-1);
        strncat(tmp_filepath, "Contents/Resources/", 256-1);
        strncat(tmp_filepath, filepath, 256-1);

        f = fopen(tmp_filepath, "r");
        if (f)
            goto retry_file;
#endif

skip_fopen:
        strncpy(tmp_filepath, filepath, 256-1);
        
        for (int i = 0; i < strlen(tmp_filepath); i++)
        {
            if (tmp_filepath[i] == '\\') {
                tmp_filepath[i] = '/';
            }
        }

        for (size_t i = 0; i < embeddedResource_aFiles_num; i++)
        {
            if (!strcmp(embeddedResource_aFiles[i].fpath, tmp_filepath)) {
                file_contents = (char*)malloc(embeddedResource_aFiles[i].data_len+1);
                if (!file_contents) {
                    if (pOutSz) {
                        *pOutSz = 0;
                    }
                    return NULL;
                }
                memcpy(file_contents, embeddedResource_aFiles[i].data, embeddedResource_aFiles[i].data_len);
                file_contents[embeddedResource_aFiles[i].data_len] = 0;

                if (pOutSz) {
                    *pOutSz = embeddedResource_aFiles[i].data_len+1;
                }

                break;
            }
        }
    }

    return file_contents;
}
// Added: appends a file's contents (if it can be read) to *pBuf (size *pLen).
static void stdEmbeddedRes_AppendFile(char** pBuf, size_t* pLen, const char* pPath)
{
    FILE* f = fopen(pPath, "rb");
    long n;
    char* pNew;
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    if (n > 0 && (pNew = (char*)realloc(*pBuf, *pLen + (size_t)n + 2)))
    {
        *pBuf = pNew;
        pNew[(*pLen)++] = '\n';
        *pLen += fread(pNew + *pLen, 1, (size_t)n, f);
        pNew[*pLen] = 0;
    }
    fclose(f);
}

// Added: the CA certificates for HTTPS/WSS (stdHttp, the web lobby): always the bundle
// built into the game, plus a resource/ssl/cacert.pem in the game folder and, on macOS,
// the system's /etc/ssl/cert.pem. A file on disk only adds roots: an old cacert.pem left
// by an earlier version used to replace the built-in one and lacked newer roots
// (e.g. GTS Root R4: "SSL peer certificate ... was not OK" for jediknight.zone on a Mac).
char* stdEmbeddedRes_LoadCaBundle(void)
{
    char* pBuf = NULL;
    size_t len = 0;
    char diskPath[256];

    for (size_t i = 0; i < embeddedResource_aFiles_num; i++)
    {
        if (!strcmp(embeddedResource_aFiles[i].fpath, "ssl/cacert.pem")) {
            pBuf = (char*)malloc(embeddedResource_aFiles[i].data_len + 1);
            if (!pBuf)
                return NULL;
            memcpy(pBuf, embeddedResource_aFiles[i].data, embeddedResource_aFiles[i].data_len);
            len = embeddedResource_aFiles[i].data_len;
            pBuf[len] = 0;
            break;
        }
    }
    if (!pBuf && !(pBuf = (char*)calloc(1, 1)))
        return NULL;

    strncpy(diskPath, "resource/ssl/cacert.pem", sizeof(diskPath) - 1);
    diskPath[sizeof(diskPath) - 1] = 0;
#ifdef FS_POSIX
    {
        char* r = (char*)malloc(strlen(diskPath) + 16);
        if (r && casepath(diskPath, r))
            strncpy(diskPath, r, sizeof(diskPath) - 1);
        free(r);
    }
#endif
#ifdef WIN32
    for (size_t i = 0; diskPath[i]; i++)
        if (diskPath[i] == '/') diskPath[i] = '\\';
#endif
    stdEmbeddedRes_AppendFile(&pBuf, &len, diskPath);
#if defined(MACOS)
    stdEmbeddedRes_AppendFile(&pBuf, &len, "/etc/ssl/cert.pem");
#endif
    return pBuf;
}
