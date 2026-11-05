#ifndef _OPENJKDF2_STDLOG_H
#define _OPENJKDF2_STDLOG_H

// Added: a log file for servers and long sessions. `-log` writes
// openjkdf2-server.log in the folder the game was started from, `-logFile <file>`
// another file; on by default with -dedicatedServer, `-noLog` turns it off.
// Everything the game prints to stdout/stderr (engine, networking transports,
// -verboseNetworking) goes into it with a timestamp per line, and still reaches
// the terminal. The file stays open across the engine's in-process restarts and
// is written line by line, so a crash loses nothing.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STDLOG_DEFAULT_FILE "openjkdf2-server.log"

// Looks through the command line for those options and starts the log if
// asked to. Call once, early in main(). Returns 1 if logging.
int stdLog_StartFromArgs(int argc, char** argv);

int stdLog_Start(const char* pPath);   // 1 on success; once per process
void stdLog_Stop(void);                // also registered with atexit
int stdLog_IsActive(void);
const char* stdLog_GetPath(void);

// One "[tag] text" line. Goes through stdout like any other print, so it is
// timestamped and shown in the terminal too. Works without a log (plain print).
void stdLog_Printf(const char* pTag, const char* pFmt, ...);
void stdLog_PrintW(const char* pTag, const uint16_t* pText);   // UTF-16 game text

// Only to the terminal, never into the log (the headless console prompt).
void stdLog_TerminalPrintf(const char* pFmt, ...);
// Only into the log, as "[tag] text" (what the terminal already shows).
void stdLog_FilePrintf(const char* pTag, const char* pFmt, ...);

// From a crash or quit signal handler: copies what is still in the capture pipe
// into the log, then the text. Avoids locks and stdio.
void stdLog_EmergencyWrite(const char* pText);

#ifdef __cplusplus
}
#endif

#endif // _OPENJKDF2_STDLOG_H
