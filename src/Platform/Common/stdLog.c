// Added: log file for servers and long sessions, see stdLog.h.
//
// How it works: stdout and stderr (file descriptors 1 and 2) are pointed at a
// pipe. A thread reads the pipe, passes everything on to the terminal as it
// was, and writes each complete line to the log file with a timestamp in
// front. Every print in the program lands in the log that way, including the
// transports' fprintf(stderr) and plain printf()s, without touching them.
// The file is written with write() per line (no stdio buffer), so it is up to
// date if the process dies. A carriage return starts the line over, as on a
// terminal.

#include "stdLog.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#if defined(PLATFORM_DPLAY) && defined(SDL2_RENDER) && !defined(ARCH_WASM) \
    && (defined(PLATFORM_LINUX) || defined(MACOS) || defined(_WIN32))
#define STDLOG_CAPTURE
#endif

#ifdef STDLOG_CAPTURE

#include "SDL2_helper.h"
#include "types.h"
#include "globals.h" // Added: g_should_exit
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#define stdLog_read _read
#define stdLog_write _write
#define stdLog_dup _dup
#define stdLog_dup2 _dup2
#define stdLog_close _close
#define stdLog_getpid _getpid
#define STDLOG_NULL_DEVICE "NUL"
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#define stdLog_read read
#define stdLog_write write
#define stdLog_dup dup
#define stdLog_dup2 dup2
#define stdLog_close close
#define stdLog_getpid getpid
#define STDLOG_NULL_DEVICE "/dev/null"
#endif

// Start a fresh file (the old one becomes <file>.1) once it grows past this.
#define STDLOG_ROTATE_BYTES (32 * 1024 * 1024)

static int stdLog_bActive = 0;
static int stdLog_logFd = -1;
static int stdLog_termOutFd = -1;   // the terminal (or whatever stdout was) before we took it
static int stdLog_termErrFd = -1;
static int stdLog_pipeReadFd = -1;
static SDL_Thread* stdLog_pThread = NULL;
static SDL_Mutex* stdLog_pMutex = NULL;
static char stdLog_aPath[1024];

static char stdLog_aLine[4096];     // reader thread only
static size_t stdLog_lineLen = 0;

static void stdLog_WriteAll(int fd, const char* p, size_t n)
{
    while (n > 0)
    {
        int ret = stdLog_write(fd, p, (unsigned int)n);
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return;
        p += ret;
        n -= (size_t)ret;
    }
}

// "2026-10-26 14:03:12.345" (local time)
static void stdLog_Timestamp(char* pOut, size_t outLen)
{
    SDL_Time now = 0;
    SDL_DateTime dt;
    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &dt, true))
    {
        snprintf(pOut, outLen, "(no clock)");
        return;
    }
    snprintf(pOut, outLen, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, dt.nanosecond / 1000000);
}

// One line into the file, with the timestamp in front.
static void stdLog_WriteLine(const char* pText, size_t len, int bLock)
{
    char aBuf[4096 + 64];
    size_t at;

    if (stdLog_logFd < 0) return;
    stdLog_Timestamp(aBuf, sizeof(aBuf));
    at = strlen(aBuf);
    aBuf[at++] = ' ';
    if (len > sizeof(aBuf) - at - 1) len = sizeof(aBuf) - at - 1;
    memcpy(aBuf + at, pText, len);
    at += len;
    aBuf[at++] = '\n';

    if (bLock && stdLog_pMutex) SDL_LockMutex(stdLog_pMutex);
    stdLog_WriteAll(stdLog_logFd, aBuf, at);
    if (bLock && stdLog_pMutex) SDL_UnlockMutex(stdLog_pMutex);
}

static void stdLog_Feed(const char* p, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        char c = p[i];
        if (c == '\n')
        {
            stdLog_WriteLine(stdLog_aLine, stdLog_lineLen, 1);
            stdLog_lineLen = 0;
        }
        else if (c == '\r')
        {
            stdLog_lineLen = 0;
        }
        else
        {
            if (stdLog_lineLen == sizeof(stdLog_aLine))
            {
                stdLog_WriteLine(stdLog_aLine, stdLog_lineLen, 1);
                stdLog_lineLen = 0;
            }
            stdLog_aLine[stdLog_lineLen++] = c;
        }
    }
}

static int SDLCALL stdLog_ReaderThread(void* pUnused)
{
    char aBuf[4096];
    (void)pUnused;
    for (;;)
    {
        int n = stdLog_read(stdLog_pipeReadFd, aBuf, sizeof(aBuf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;   // every write end closed: stdLog_Stop
        if (stdLog_termOutFd >= 0)
            stdLog_WriteAll(stdLog_termOutFd, aBuf, (size_t)n);
        stdLog_Feed(aBuf, (size_t)n);
    }
    if (stdLog_lineLen)
    {
        stdLog_WriteLine(stdLog_aLine, stdLog_lineLen, 1);
        stdLog_lineLen = 0;
    }
    return 0;
}

void stdLog_EmergencyWrite(const char* pText)
{
    if (!stdLog_bActive) return;
#ifndef _WIN32
    // Give the reader thread a moment to empty the pipe (it is still running
    // after a quit signal, and usually after a crash in another thread too).
    {
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        int waiting = 0;
        for (int i = 0; i < 30; i++)
        {
            if (ioctl(stdLog_pipeReadFd, FIONREAD, &waiting) != 0 || waiting <= 0) break;
            nanosleep(&ts, NULL);
        }
        nanosleep(&ts, NULL);   // and to write the line it just read
    }
    // If it didn't, take the rest ourselves.
    {
        char aBuf[4096];
        char aLine[1024];
        size_t lineLen = 0;
        int flags = fcntl(stdLog_pipeReadFd, F_GETFL);
        fcntl(stdLog_pipeReadFd, F_SETFL, flags | O_NONBLOCK);
        for (;;)
        {
            ssize_t n = read(stdLog_pipeReadFd, aBuf, sizeof(aBuf));
            if (n <= 0) break;
            if (stdLog_termOutFd >= 0) stdLog_WriteAll(stdLog_termOutFd, aBuf, (size_t)n);
            for (ssize_t i = 0; i < n; i++)
            {
                if (aBuf[i] == '\n' || lineLen == sizeof(aLine))
                {
                    stdLog_WriteLine(aLine, lineLen, 0);
                    lineLen = 0;
                }
                if (aBuf[i] == '\r') lineLen = 0;
                else if (aBuf[i] != '\n') aLine[lineLen++] = aBuf[i];
            }
        }
        if (lineLen) stdLog_WriteLine(aLine, lineLen, 0);
        fcntl(stdLog_pipeReadFd, F_SETFL, flags);
    }
#endif
    stdLog_WriteLine(pText, strlen(pText), 0);
    if (stdLog_termErrFd >= 0)
    {
        stdLog_WriteAll(stdLog_termErrFd, pText, strlen(pText));
        stdLog_WriteAll(stdLog_termErrFd, "\n", 1);
    }
}

// The first quit signal asks the game to exit (g_should_exit, as closing the
// window does), so it leaves a multiplayer game properly on the way out and
// the other players aren't left waiting for a timeout; a second one stops it
// at once, in case that hangs.
static void stdLog_QuitSignal(int sig)
{
    static volatile sig_atomic_t bAsked = 0;
    const char* pText = "[log] stopping: SIGINT (Ctrl+C); again to stop at once";
    if (sig == SIGTERM) pText = "[log] stopping: SIGTERM; again to stop at once";
#ifdef SIGHUP
    else if (sig == SIGHUP) pText = "[log] stopping: SIGHUP (terminal closed)";
#endif
    if (!bAsked)
    {
        bAsked = 1;
        stdLog_EmergencyWrite(pText);
        g_should_exit = 1;
        return;
    }
    stdLog_EmergencyWrite("[log] stopped at once by a second signal");
    signal(sig, SIG_DFL);
    raise(sig);
}

int stdLog_IsActive(void)
{
    return stdLog_bActive;
}

const char* stdLog_GetPath(void)
{
    return stdLog_bActive ? stdLog_aPath : "";
}

static void stdLog_Rotate(const char* pPath)
{
    struct stat st;
    char aOld[1040];
    if (stat(pPath, &st) != 0 || st.st_size < STDLOG_ROTATE_BYTES) return;
    snprintf(aOld, sizeof(aOld), "%s.1", pPath);
    remove(aOld);
    rename(pPath, aOld);
}

int stdLog_Start(const char* pPath)
{
    int aPipe[2];
    char aStamp[64];
    char aHeader[1400];

    if (stdLog_bActive) return 1;
    if (!pPath || !pPath[0]) pPath = STDLOG_DEFAULT_FILE;

#ifdef _WIN32
    if (!_fullpath(stdLog_aPath, pPath, sizeof(stdLog_aPath)))
        snprintf(stdLog_aPath, sizeof(stdLog_aPath), "%s", pPath);
#else
    if (pPath[0] == '/' || !getcwd(stdLog_aPath, sizeof(stdLog_aPath)))
        snprintf(stdLog_aPath, sizeof(stdLog_aPath), "%s", pPath);
    else
    {
        size_t at = strlen(stdLog_aPath);
        snprintf(stdLog_aPath + at, sizeof(stdLog_aPath) - at, "/%s", pPath);
    }
#endif

    stdLog_Rotate(stdLog_aPath);
#ifdef _WIN32
    stdLog_logFd = _open(stdLog_aPath, _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    stdLog_logFd = open(stdLog_aPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
#endif
    if (stdLog_logFd < 0)
    {
        fprintf(stderr, "OpenJKDF2: can't open the log file %s: %s\n", stdLog_aPath, strerror(errno));
        return 0;
    }

    fflush(stdout);
    fflush(stderr);
    stdLog_termOutFd = stdLog_dup(1);   // -1 if there is no stdout (Windows without a console)
    stdLog_termErrFd = stdLog_dup(2);
#ifdef _WIN32
    if (_pipe(aPipe, 65536, _O_BINARY) != 0)
#else
    if (pipe(aPipe) != 0)
#endif
    {
        fprintf(stderr, "OpenJKDF2: can't start the log: %s\n", strerror(errno));
        stdLog_close(stdLog_logFd);
        stdLog_logFd = -1;
        return 0;
    }
#ifndef _WIN32
    fcntl(aPipe[0], F_SETFD, FD_CLOEXEC);
    if (stdLog_termOutFd >= 0) fcntl(stdLog_termOutFd, F_SETFD, FD_CLOEXEC);
    if (stdLog_termErrFd >= 0) fcntl(stdLog_termErrFd, F_SETFD, FD_CLOEXEC);
#endif
    stdLog_pipeReadFd = aPipe[0];
    stdLog_dup2(aPipe[1], 1);
    stdLog_dup2(aPipe[1], 2);
    stdLog_close(aPipe[1]);
#ifdef _WIN32
    // The Windows CRT has no line buffering (_IOLBF means full buffering there).
    setvbuf(stdout, NULL, _IONBF, 0);
#else
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
#endif
    setvbuf(stderr, NULL, _IONBF, 0);

    stdLog_Timestamp(aStamp, sizeof(aStamp));
    snprintf(aHeader, sizeof(aHeader), "\n%s ==== OpenJKDF2 %s (%s), log started, process %d ====\n",
             aStamp, openjkdf2_aReleaseVersion, openjkdf2_aReleaseCommitShort, (int)stdLog_getpid());
    stdLog_WriteAll(stdLog_logFd, aHeader, strlen(aHeader));

    stdLog_pMutex = SDL_CreateMutex();
    stdLog_pThread = SDL_CreateThread(stdLog_ReaderThread, "stdLog", NULL);
    stdLog_bActive = 1;
    atexit(stdLog_Stop);

    signal(SIGINT, stdLog_QuitSignal);
    signal(SIGTERM, stdLog_QuitSignal);
#ifdef SIGHUP
    signal(SIGHUP, stdLog_QuitSignal);
#endif

    stdLog_Printf("log", "writing to %s", stdLog_aPath);
    return 1;
}

void stdLog_Stop(void)
{
    if (!stdLog_bActive) return;
    stdLog_Printf("log", "log closed");
    fflush(stdout);
    fflush(stderr);

    // Put stdout/stderr back; that closes the pipe's last write ends, the
    // reader sees the end of it and finishes.
    if (stdLog_termOutFd < 0 || stdLog_termErrFd < 0)
    {
#ifdef _WIN32
        int nul = _open(STDLOG_NULL_DEVICE, _O_WRONLY);
#else
        int nul = open(STDLOG_NULL_DEVICE, O_WRONLY);
#endif
        if (stdLog_termOutFd < 0 && nul >= 0) stdLog_dup2(nul, 1);
        if (stdLog_termErrFd < 0 && nul >= 0) stdLog_dup2(nul, 2);
        if (nul >= 0) stdLog_close(nul);
    }
    if (stdLog_termOutFd >= 0) stdLog_dup2(stdLog_termOutFd, 1);
    if (stdLog_termErrFd >= 0) stdLog_dup2(stdLog_termErrFd, 2);

    if (stdLog_pThread) SDL_WaitThread(stdLog_pThread, NULL);
    stdLog_pThread = NULL;
    stdLog_bActive = 0;

    stdLog_close(stdLog_pipeReadFd);
    stdLog_close(stdLog_logFd);
    if (stdLog_termOutFd >= 0) stdLog_close(stdLog_termOutFd);
    if (stdLog_termErrFd >= 0) stdLog_close(stdLog_termErrFd);
    stdLog_pipeReadFd = stdLog_logFd = stdLog_termOutFd = stdLog_termErrFd = -1;
    if (stdLog_pMutex) SDL_DestroyMutex(stdLog_pMutex);
    stdLog_pMutex = NULL;
}

void stdLog_TerminalPrintf(const char* pFmt, ...)
{
    char aBuf[512];
    va_list ap;
    va_start(ap, pFmt);
    if (!stdLog_bActive)
    {
        vprintf(pFmt, ap);
        fflush(stdout);
    }
    else if (stdLog_termOutFd >= 0)
    {
        int n = vsnprintf(aBuf, sizeof(aBuf), pFmt, ap);
        if (n > 0) stdLog_WriteAll(stdLog_termOutFd, aBuf, (size_t)n < sizeof(aBuf) ? (size_t)n : sizeof(aBuf) - 1);
    }
    va_end(ap);
}

void stdLog_FilePrintf(const char* pTag, const char* pFmt, ...)
{
    char aBuf[1024];
    va_list ap;
    int n;
    if (!stdLog_bActive) return;
    n = snprintf(aBuf, sizeof(aBuf), "[%s] ", pTag);
    va_start(ap, pFmt);
    vsnprintf(aBuf + n, sizeof(aBuf) - n, pFmt, ap);
    va_end(ap);
    stdLog_WriteLine(aBuf, strlen(aBuf), 1);
}

int stdLog_StartFromArgs(int argc, char** argv)
{
    const char* pPath = NULL;
    int bWanted = 0;

    for (int i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        if (!a || (a[0] != '-' && a[0] != '/')) continue;
        a++;
        if (!SDL_strcasecmp(a, "noLog")) return 0;
        if (!SDL_strcasecmp(a, "dedicatedServer")) bWanted = 1;
        if (!SDL_strcasecmp(a, "log")) bWanted = 1;
        if (!SDL_strcasecmp(a, "logFile") && i + 1 < argc)
        {
            bWanted = 1;
            pPath = argv[++i];
        }
    }
    if (!bWanted) return 0;
    if (!stdLog_Start(pPath)) return 0;

    // Then the command line, for the record.
    {
        char aCmd[1024];
        size_t at = 0;
        aCmd[0] = 0;
        for (int i = 1; i < argc && at < sizeof(aCmd) - 1; i++)
        {
            if (!argv[i] || !argv[i][0]) continue;
            at += (size_t)snprintf(aCmd + at, sizeof(aCmd) - at, "%s%s", at ? " " : "", argv[i]);
        }
        stdLog_Printf("log", "command line: %s", aCmd[0] ? aCmd : "(none)");
    }
    return 1;
}

#else // !STDLOG_CAPTURE

int stdLog_StartFromArgs(int argc, char** argv) { (void)argc; (void)argv; return 0; }
int stdLog_Start(const char* pPath) { (void)pPath; return 0; }
void stdLog_Stop(void) {}
int stdLog_IsActive(void) { return 0; }
const char* stdLog_GetPath(void) { return ""; }
void stdLog_EmergencyWrite(const char* pText) { (void)pText; }
void stdLog_FilePrintf(const char* pTag, const char* pFmt, ...) { (void)pTag; (void)pFmt; }

void stdLog_TerminalPrintf(const char* pFmt, ...)
{
    va_list ap;
    va_start(ap, pFmt);
    vprintf(pFmt, ap);
    va_end(ap);
    fflush(stdout);
}

#endif // STDLOG_CAPTURE

void stdLog_Printf(const char* pTag, const char* pFmt, ...)
{
    char aBuf[1024];
    va_list ap;
    size_t len;

    va_start(ap, pFmt);
    vsnprintf(aBuf, sizeof(aBuf), pFmt, ap);
    va_end(ap);
    len = strlen(aBuf);
    while (len && aBuf[len - 1] == '\n') aBuf[--len] = 0;
    printf("[%s] %s\n", pTag, aBuf);
}

void stdLog_PrintW(const char* pTag, const uint16_t* pText)
{
    char aBuf[1024];
    size_t at = 0;

    for (size_t i = 0; pText && pText[i] && at < sizeof(aBuf) - 5; i++)
    {
        uint32_t c = pText[i];
        if (c >= 0xD800 && c < 0xDC00 && pText[i + 1] >= 0xDC00 && pText[i + 1] < 0xE000)
        {
            c = 0x10000 + ((c - 0xD800) << 10) + (pText[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) aBuf[at++] = (char)c;
        else if (c < 0x800)
        {
            aBuf[at++] = (char)(0xC0 | (c >> 6));
            aBuf[at++] = (char)(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            aBuf[at++] = (char)(0xE0 | (c >> 12));
            aBuf[at++] = (char)(0x80 | ((c >> 6) & 0x3F));
            aBuf[at++] = (char)(0x80 | (c & 0x3F));
        }
        else
        {
            aBuf[at++] = (char)(0xF0 | (c >> 18));
            aBuf[at++] = (char)(0x80 | ((c >> 12) & 0x3F));
            aBuf[at++] = (char)(0x80 | ((c >> 6) & 0x3F));
            aBuf[at++] = (char)(0x80 | (c & 0x3F));
        }
    }
    aBuf[at] = 0;
    stdLog_Printf(pTag, "%s", aBuf);
}
