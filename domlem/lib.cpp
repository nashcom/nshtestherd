/* lib.cpp - small helpers shared by the domlem modules */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

#include <random>

#ifdef UNIX
#include <dirent.h>
#include <signal.h>
#include <sys/types.h>
#endif

#include <global.h>
#include <addin.h>
#include <lookup.h>
#include <names.h>
#include <osmem.h>
#include <idtable.h>
#include <miscerr.h>
#include <nsfdb.h>
#include <nsfnote.h>
#include <nsfsearc.h>
#include <ods.h>
#include <odstypes.h>
#include <editods.h>
#include <fontid.h>
#include <easycd.h>
#include <osmisc.h>
#include <osfile.h>

#include "lib.h"


void CloseDb (DBHANDLE *phDb)
{
    if ((NULL == phDb) || (NULLHANDLE == *phDb))
        goto Done;

    /* NSFDbCloseSession always closes the handle, and the session too when no other database of the process is open on
     * it. So it is the one close, never followed by NSFDbClose. */
    if (g_bCloseSession)
        NSFDbCloseSession (*phDb);
    else
        NSFDbClose (*phDb);

    *phDb = NULLHANDLE;

Done:

    return;
}


BOOL IsNullStr (const char *pszStr)
{
    if (NULL == pszStr)
        return TRUE;

    if ('\0' == *pszStr)
        return TRUE;

    return FALSE;
}


char *NullIfEmpty (char *pszStr)
{
    return IsNullStr (pszStr) ? NULL : pszStr;
}


void CopyStr (char *pszDest, size_t DestSize, const char *pszSource)
{
    int nLen = 0;

    if ((NULL == pszDest) || (0 == DestSize))
        goto Done;

    nLen = snprintf (pszDest, DestSize, "%s", pszSource ? pszSource : "");

    if ((nLen < 0) || ((size_t) nLen >= DestSize))
        AddInLogMessageText ("%s: Warning: a value was cut to %lu characters", NOERROR, g_szLogPrefix, (unsigned long) (DestSize - 1));

Done:

    return;
}


BOOL IsSafeName (const char *pszName, size_t nMaxLen)
{
    BOOL   bSafe   = TRUE;
    size_t nLength = 0;

    if (IsNullStr (pszName))
        goto Done;

    for (const char *p = pszName; *p; p++)
    {
        nLength++;

        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || '.' == *p || '-' == *p || '_' == *p))
            bSafe = FALSE;
    }

    if ((nLength > nMaxLen) || (NULL != strstr (pszName, "..")))
        bSafe = FALSE;

Done:

    return IsNullStr (pszName) ? FALSE : bSafe;
}


/* The process id in the name of a domlem temporary file: "domlem_att_<pid>_<mail>_<n>.tmp" or "domlem_<test_id>_<pid>.id".
 * FALSE for any other name. */

static BOOL TempFilePid (const char *pszName, DWORD *pdwPid)
{
    BOOL        bOK   = FALSE;
    std::string Name  = pszName ? pszName : "";
    size_t      nFrom = std::string::npos;
    size_t      nTo   = std::string::npos;

    if ((0 == Name.compare (0, 11, "domlem_att_")) && (Name.size() > 15) && (0 == Name.compare (Name.size() - 4, 4, ".tmp")))
    {
        nFrom = 11;
        nTo   = Name.find ('_', nFrom);
    }
    else if ((0 == Name.compare (0, 7, "domlem_")) && (Name.size() > 10) && (0 == Name.compare (Name.size() - 3, 3, ".id")))
    {
        nTo   = Name.size() - 3;
        nFrom = Name.rfind ('_', nTo);

        if (std::string::npos != nFrom)
            nFrom++;
    }

    if ((std::string::npos == nFrom) || (std::string::npos == nTo) || (nTo <= nFrom))
        goto Done;

    bOK = ParseUnsigned (Name.substr (nFrom, nTo - nFrom).c_str(), 0xFFFFFFFF, pdwPid) && (*pdwPid > 1);

Done:

    return bOK;
}


void RemoveStaleTempFiles()
{
#ifdef UNIX
    DIR           *pDir      = NULL;
    struct dirent *pEntry    = NULL;
    DWORD          dwPid     = 0;
    DWORD          dwRemoved = 0;
    char           szDir[MAXPATH+1]      = {0};
    char           szPath[MAXPATH*2+2]   = {0};

    OSGetDataDirectory (szDir);

    pDir = opendir (szDir);

    if (NULL == pDir)
        goto Done;

    while (NULL != (pEntry = readdir (pDir)))
    {
        if (!TempFilePid (pEntry->d_name, &dwPid) || ((unsigned long) dwPid == DOMLEM_GETPID()))
            continue;

        /* The process is alive (or belongs to somebody else: EPERM): not ours to remove */
        if ((0 == kill ((pid_t) dwPid, 0)) || (ESRCH != errno))
            continue;

        snprintf (szPath, sizeof (szPath), "%s/%s", szDir, pEntry->d_name);

        if (0 == remove (szPath))
            dwRemoved++;
    }

    if (dwRemoved)
        AddInLogMessageText ("%s: Removed %lu temporary files left by lemmings that ended without cleaning up", NOERROR, g_szLogPrefix, (unsigned long) dwRemoved);

Done:

    if (pDir)
        closedir (pDir);
#endif
}


BOOL ParseUnsigned (const char *pszText, DWORD dwMax, DWORD *pdwValue)
{
    BOOL          bOK     = FALSE;
    char         *pszEnd  = NULL;
    unsigned long ulValue = 0;

    if (IsNullStr (pszText) || (NULL == pdwValue) || ('-' == *pszText) || ('+' == *pszText))
        goto Done;

    errno   = 0;
    ulValue = strtoul (pszText, &pszEnd, 10);

    if ((0 != errno) || (NULL == pszEnd) || (*pszEnd) || (ulValue > (unsigned long) dwMax))
        goto Done;

    *pdwValue = (DWORD) ulValue;
    bOK       = TRUE;

Done:

    return bOK;
}


BOOL ParseRange (const char *pszText, DWORD dwLimit, ValueRange *pRange)
{
    BOOL        bOK    = FALSE;
    DWORD       dwFrom = 0;
    DWORD       dwTo   = 0;
    std::string Text   = pszText ? pszText : "";
    size_t      nDash  = Text.find ('-');

    if (NULL == pRange)
        goto Done;

    if (std::string::npos == nDash)
    {
        if (!ParseUnsigned (Text.c_str(), dwLimit, &dwFrom))
            goto Done;

        dwTo = dwFrom;
    }
    else
    {
        if (!ParseUnsigned (Text.substr (0, nDash).c_str(), dwLimit, &dwFrom) || !ParseUnsigned (Text.substr (nDash + 1).c_str(), dwLimit, &dwTo) || (dwFrom > dwTo))
            goto Done;
    }

    pRange->dwMin = dwFrom;
    pRange->dwMax = dwTo;
    bOK           = TRUE;

Done:

    return bOK;
}


DWORD PickInRange (const ValueRange &Range, unsigned long ulSeed)
{
    DWORD dwValue = Range.dwMin;

    if (Range.dwMax > Range.dwMin)
    {
        std::mt19937 Random ((std::mt19937::result_type) ulSeed);
        dwValue = std::uniform_int_distribution<DWORD> (Range.dwMin, Range.dwMax) (Random);
    }

    return dwValue;
}


std::string RangeText (const ValueRange &Range)
{
    if (Range.dwMax == Range.dwMin)
        return std::to_string ((unsigned long) Range.dwMin);

    return std::to_string ((unsigned long) Range.dwMin) + "-" + std::to_string ((unsigned long) Range.dwMax);
}


std::string PercentDecode (const char *pszText)
{
    std::string Decoded;
    const char *p = pszText ? pszText : "";

    while (*p)
    {
        if (('%' == p[0]) && isxdigit ((unsigned char) p[1]) && isxdigit ((unsigned char) p[2]))
        {
            char szHex[3] = { p[1], p[2], '\0' };

            Decoded += (char) strtol (szHex, NULL, 16);
            p += 3;
        }
        else
        {
            Decoded += *p;
            p++;
        }
    }

    return Decoded;
}


std::vector<OptionPair> SplitUrlOptions (const char *pszText)
{
    std::vector<OptionPair> Options;
    std::vector<std::string> Parts = SplitList (pszText, '&');

    for (size_t nPart = 0; nPart < Parts.size(); nPart++)
    {
        OptionPair Option;
        size_t     nEqual = Parts[nPart].find ('=');

        Option.name = PercentDecode (Parts[nPart].substr (0, nEqual).c_str());

        if (std::string::npos != nEqual)
        {
            Option.value     = PercentDecode (Parts[nPart].substr (nEqual + 1).c_str());
            Option.bHasValue = TRUE;
        }

        if (!Option.name.empty())
            Options.push_back (Option);
    }

    return Options;
}


std::vector<std::string> SplitList (const char *pszList, char chDelimiter)
{
    std::vector<std::string> Entries;
    std::string              Rest  = pszList ? pszList : "";
    size_t                   nPos  = 0;
    size_t                   nNext = 0;

    while (nPos <= Rest.size())
    {
        nNext = Rest.find (chDelimiter, nPos);

        if (std::string::npos == nNext)
            nNext = Rest.size();

        std::string Entry = Rest.substr (nPos, nNext - nPos);
        size_t      nFrom = Entry.find_first_not_of (" \t");
        size_t      nTo   = Entry.find_last_not_of (" \t");

        if (std::string::npos != nFrom)
            Entries.push_back (Entry.substr (nFrom, nTo - nFrom + 1));

        nPos = nNext + 1;
    }

    return Entries;
}


std::string ErrorText (STATUS error)
{
    char szBuffer[256] = {0};
    char szCode[32]    = {0};

    OSLoadString (NULLHANDLE, ERR(error), szBuffer, (WORD) sizeofstring (szBuffer));
    snprintf (szCode, sizeof (szCode), " (0x%04X)", (unsigned) ERR(error));

    return std::string (szBuffer) + szCode;
}


/* Is there a person document for this name, short name or internet address? Returns TRUE when found and, when the document has a name, its full name. */

BOOL LookupUser (const char *pszServer, const char *pszKey, char *pszRetFullName, WORD wMaxFullName)
{
    DHANDLE hLookup    = NULLHANDLE;
    char   *pLookup    = NULL;
    char   *pName      = NULL;
    char   *pMatch     = NULL;
    WORD    wMatches   = 0;
    BOOL    bFound     = FALSE;
    char    szItems[]  = "FullName\0";    /* the list of items to return, separated and ended by \0 */

    if (pszRetFullName && wMaxFullName)
        *pszRetFullName = '\0';

    if (IsNullStr (pszKey))
        goto Done;

    if (NOERROR != NAMELookup (pszServer, NAME_LOOKUP_ALL | NAME_LOOKUP_NOSEARCHING, 1, USERNAMESSPACE, 1, pszKey, 1, szItems, &hLookup))
    {
        hLookup = NULLHANDLE;
        goto Done;
    }

    pLookup = (char *) OSLockObject (hLookup);

    if (NULL == pLookup)
        goto Done;

    pName = (char *) NAMELocateNextName (pLookup, NULL, &wMatches);

    if ((NULL == pName) || (0 == wMatches))
        goto Done;

    pMatch = (char *) NAMELocateNextMatch (pLookup, pName, pMatch);

    if (NULL == pMatch)
        goto Done;

    /* When the caller wants the name, a match without a usable full name does not count as found */
    if (pszRetFullName && wMaxFullName)
    {
        if ((NOERROR != NAMEGetTextItem (pMatch, 0, 0, pszRetFullName, (WORD) (wMaxFullName - 1))) || ('\0' == *pszRetFullName))
        {
            *pszRetFullName = '\0';
            goto Done;
        }
    }

    bFound = TRUE;

Done:

    if (pLookup)
        OSUnlockObject (hLookup);

    if (hLookup)
        OSMemFree (hLookup);

    return bFound;
}


BOOL LookupMailFile (const char *pszServer, const char *pszUserName, char *pszRetMailServer, WORD wMaxMailServer, char *pszRetMailFile, WORD wMaxMailFile)
{
    DHANDLE hLookup    = NULLHANDLE;
    char   *pLookup    = NULL;
    char   *pName      = NULL;
    char   *pMatch     = NULL;
    WORD    wMatches   = 0;
    BOOL    bFound     = FALSE;
    char    szItems[]  = "MailServer\0MailFile";    /* the items to return, in this order, each ended by \0 */

    if ((NULL == pszRetMailServer) || (0 == wMaxMailServer) || (NULL == pszRetMailFile) || (0 == wMaxMailFile))
        goto Done;

    *pszRetMailServer = '\0';
    *pszRetMailFile   = '\0';

    if (IsNullStr (pszUserName))
        goto Done;

    if (NOERROR != NAMELookup (pszServer, NAME_LOOKUP_ALL | NAME_LOOKUP_NOSEARCHING, 1, USERNAMESSPACE, 1, pszUserName, 2, szItems, &hLookup))
    {
        hLookup = NULLHANDLE;
        goto Done;
    }

    pLookup = (char *) OSLockObject (hLookup);

    if (NULL == pLookup)
        goto Done;

    pName = (char *) NAMELocateNextName (pLookup, NULL, &wMatches);

    if ((NULL == pName) || (0 == wMatches))
        goto Done;

    pMatch = (char *) NAMELocateNextMatch (pLookup, pName, pMatch);

    if (NULL == pMatch)
        goto Done;

    /* Item 0: MailServer (may be empty), item 1: MailFile (required) */
    if (NOERROR != NAMEGetTextItem (pMatch, 0, 0, pszRetMailServer, (WORD) (wMaxMailServer - 1)))
        *pszRetMailServer = '\0';

    if ((NOERROR != NAMEGetTextItem (pMatch, 1, 0, pszRetMailFile, (WORD) (wMaxMailFile - 1))) || ('\0' == *pszRetMailFile))
    {
        *pszRetMailFile = '\0';
        goto Done;
    }

    bFound = TRUE;

Done:

    if (pLookup)
        OSUnlockObject (hLookup);

    if (hLookup)
        OSMemFree (hLookup);

    return bFound;
}


/* NSFSearch callback: adds the note ID of every note that matches the formula to the ID table (parameter: DHANDLE *) */

static STATUS AddIDUnique (void *phNoteIDTable, SEARCH_MATCH *pSearchInfo, ITEM_TABLE *pSummaryInfo)
{
    DHANDLE      hNoteIDTable = NULLHANDLE;
    STATUS       error        = NOERROR;
    BOOL         bInserted    = FALSE;
    SEARCH_MATCH SearchMatch  = {0};

    if (NULL == pSearchInfo)
    {
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    memcpy ((char *) (&SearchMatch), (char *) pSearchInfo, sizeof (SEARCH_MATCH));

    /* Only notes that match the formula */
    if (!(SearchMatch.SERetFlags & SE_FMATCH))
        goto Done;

    if (phNoteIDTable)
    {
        hNoteIDTable = *((DHANDLE *) phNoteIDTable);

        if (hNoteIDTable)
            error = IDInsert (hNoteIDTable, SearchMatch.ID.NoteID, &bInserted);
    }

Done:

    return ERR (error);
}


STATUS GetDocsByFormula (DBHANDLE hDb, const char *pszFormula, const char *pszViewTitle, DHANDLE *phNoteIDTable, DWORD *pdwEntries)
{
    STATUS        error        = NOERROR;
    WORD          wdc          = 0;
    WORD          wFormulaLen  = 0;
    BOOL          bCreated     = FALSE;
    FORMULAHANDLE hFormula     = NULLHANDLE;

    if (pdwEntries)
        *pdwEntries = 0;

    if ((NULLHANDLE == hDb) || IsNullStr (pszFormula) || (NULL == phNoteIDTable))
    {
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    if (NULLHANDLE == *phNoteIDTable)
    {
        error = IDCreateTable (sizeof (NOTEID), phNoteIDTable);

        if (error)
        {
            *phNoteIDTable = NULLHANDLE;
            goto Done;
        }

        bCreated = TRUE;
    }

    error = NSFFormulaCompile (NULL, 0, pszFormula, (WORD) strlen (pszFormula), &hFormula, &wFormulaLen, &wdc, &wdc, &wdc, &wdc, &wdc);

    if (error)
    {
        AddInLogMessageText ("%s: Error compiling search formula [%s]", error, g_szLogPrefix, pszFormula);
        hFormula = NULLHANDLE;
        goto Done;
    }

    error = NSFSearch (hDb,                                /* database handle */
                       hFormula,                           /* selection formula */
                       (char *) pszViewTitle,              /* title of view in selection formula, NULL: none */
                       0,                                  /* search flags */
                       NOTE_CLASS_DOCUMENT,                /* note class to find */
                       NULL,                               /* starting date (unused) */
                       AddIDUnique,                        /* called for each note found */
                       phNoteIDTable,                      /* argument to AddIDUnique */
                       NULL);                              /* returned ending date (unused) */

    if (error)
    {
        AddInLogMessageText ("%s: Error searching documents [%s]", error, g_szLogPrefix, pszFormula);
        goto Done;
    }

    if (pdwEntries)
        *pdwEntries = IDEntries (*phNoteIDTable);

Done:

    if (hFormula)
        OSMemFree (hFormula);

    /* An ID table created here is of no use to the caller on an error */
    if (error && bCreated)
    {
        IDDestroyTable (*phNoteIDTable);
        *phNoteIDTable = NULLHANDLE;
    }

    return error;
}


STATUS GetRandomDocByFormula (DBHANDLE hDb, const char *pszFormula, const char *pszViewTitle, NOTEID *pRetNoteID, DWORD *pdwMatches)
{
    static std::mt19937 Generator ((std::random_device()()));

    STATUS error      = NOERROR;
    DHANDLE hTable    = NULLHANDLE;
    DWORD  dwEntries  = 0;
    DWORD  dwPick     = 0;
    DWORD  dwIndex    = 0;
    DWORD  dwNoteID   = 0;
    BOOL   bMore      = FALSE;

    if (pRetNoteID)
        *pRetNoteID = 0;

    if (pdwMatches)
        *pdwMatches = 0;

    if (NULL == pRetNoteID)
    {
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    error = GetDocsByFormula (hDb, pszFormula, pszViewTitle, &hTable, &dwEntries);

    if (error)
        goto Done;

    if (pdwMatches)
        *pdwMatches = dwEntries;

    if (0 == dwEntries)
    {
        error = ERR_NOT_FOUND;
        goto Done;
    }

    /* The ID table is sorted, not indexed: walk to the picked entry */
    dwPick = (DWORD) std::uniform_int_distribution<DWORD> (0, dwEntries - 1) (Generator);
    bMore  = IDScan (hTable, TRUE, &dwNoteID);

    for (dwIndex = 0; bMore && (dwIndex < dwPick); dwIndex++)
        bMore = IDScan (hTable, FALSE, &dwNoteID);

    if (!bMore)
    {
        error = ERR_NOT_FOUND;
        goto Done;
    }

    *pRetNoteID = (NOTEID) dwNoteID;

Done:

    if (hTable)
        IDDestroyTable (hTable);

    return error;
}


/* Actual value stored in the CDBITMAPSEGMENT to generate the icon */
static unsigned char g_BitmapSegment[] =
{
    0x40, 0x04, 0x00, 0xC4, 0x00, 0x40, 0x02, 0x01, 0xC3, 0x02, 0xC9, 0x00, 0xC4, 0x00, 0x01, 0x02,
    0x40, 0x02, 0x02, 0x01, 0x00, 0xC2, 0x02, 0xC8, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x03,
    0x04, 0x01, 0x02, 0x01, 0x02, 0xC7, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x02, 0x02, 0x00,
    0x02, 0xC2, 0x01, 0x01, 0x02, 0xC6, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x03, 0x02, 0x01,
    0x02, 0xC3, 0x01, 0x01, 0x02, 0xC5, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x02, 0x02, 0x00,
    0x02, 0xC4, 0x01, 0x01, 0x02, 0xC4, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x03, 0x01, 0x01,
    0xC7, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x02, 0x08, 0x00, 0x01, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x03, 0x08, 0x01, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x02, 0x08,
    0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02,
    0x03, 0x08, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02,
    0x40, 0x02, 0x02, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0xC3, 0x00, 0xC4, 0x00,
    0x01, 0x02, 0x40, 0x02, 0x03, 0x08, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0xC3, 0x00,
    0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x02, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02,
    0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x03, 0x08, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x01, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x02, 0x08, 0x00, 0x01, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x03, 0x08, 0x01, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x02, 0x08,
    0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02,
    0x03, 0x08, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02,
    0x40, 0x02, 0x02, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0xC3, 0x00, 0xC4, 0x00,
    0x01, 0x02, 0x40, 0x02, 0x03, 0x08, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0xC3, 0x00,
    0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x02, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02,
    0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x03, 0x08, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x01, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x02, 0x08, 0x00, 0x01, 0x00, 0x01,
    0x00, 0x01, 0x00, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x03, 0x08, 0x01, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02, 0x02, 0x08,
    0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02, 0x40, 0x02,
    0x03, 0x08, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0xC3, 0x00, 0xC4, 0x00, 0x01, 0x02,
    0x40, 0x02, 0x02, 0x08, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0xC3, 0x00, 0xC4, 0x00,
    0x01, 0x02, 0x40, 0x02, 0x03, 0x08, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x02, 0xC3, 0x00,
    0xC4, 0x00, 0x40, 0x03, 0x01, 0x01, 0x02, 0xC3, 0x00, 0x40, 0x04, 0x00
};


/* Actual color value stored in the CDCOLORTABLE, the colors of the icon */
static unsigned char g_ColorTableEntries[] = { 0xFF, 0xFF, 0xFF, 0xC8, 0xC8, 0xC8, 0x00, 0x00, 0x00 };


/* Actual pattern table entries stored in the CDPATTERNTABLE, needed for the icon */
static unsigned char g_PatternTableEntries[] =
{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFA, 0xFF, 0xDD, 0xDD, 0x2E, 0x16, 0xDD, 0xDD,
    0x3C, 0xC6, 0x00, 0x00, 0x5C, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x39, 0x38, 0x30, 0x30, 0x36, 0x43, 0x41, 0x36, 0x41, 0x43, 0x2F, 0x2F, 0x31, 0x45, 0x32, 0x00,
    0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
    0xFE, 0xFF, 0x04, 0x00, 0xFA, 0xFF, 0xDD, 0xDD, 0x2E, 0x16, 0xDD, 0xDD, 0xD8, 0x7C, 0x04, 0x00
};

/* Values needed to define the CDBITMAPHEADER */
#define BM_HDR_WH       32      /* pixels */
#define BM_HDR_BP       8       /* bits per pixel */
#define BM_HDR_SP       1       /* samples per pixel */
#define BM_HDR_SEG      1       /* number of segments */
#define BM_HDR_CC       3       /* color count */
#define BM_HDR_PC       4       /* pattern count */

/* Values needed to define the CDBITMAPSEGMENT */
#define BM_SEG_SL       32      /* scan line count */

/* Room for the icon records themselves: bitmap, color and pattern tables and the record headers */
#define ICON_RECORDS_SIZE   4096


/* Writes one record at the buffer position and moves the position. The records are padded to an even length. */

static void WriteRecord (char **ppBuffer, WORD wType, void *pRecord, DWORD dwLength)
{
    ODSWriteMemory ((void **) ppBuffer, wType, pRecord, 1);

    if (dwLength % 2)
        *(*ppBuffer)++ = '\0';
}


STATUS AddAttachmentHotspot (DHANDLE hCompound, const char *pszAttachmentName, const char *pszLabelText, std::string &Err)
{
    STATUS  error         = NOERROR;
    WORD    wFileNameLen  = 0;
    WORD    wFileLabelLen = 0;

    std::vector<char> Buffer;
    char *pCDBuffer       = NULL;
    char *pCDBufferStart  = NULL;

    CDHOTSPOTBEGIN  CDHotSpotBegin  = {0};
    CDHOTSPOTEND    CDHotSpotEnd    = {0};
    CDGRAPHIC       CdGraphic       = {0};
    CDBITMAPHEADER  CdBitMapHeader  = {0};
    CDBITMAPSEGMENT CdBitMapSegment = {0};
    CDCOLORTABLE    CdColorTable    = {0};
    CDPATTERNTABLE  CdPatternTable  = {0};
    CDTEXT          CDText          = {0};

    if ((NULLHANDLE == hCompound) || IsNullStr (pszAttachmentName) || (strlen (pszAttachmentName) > 255))
    {
        Err   = "No compound text or no usable attachment name";
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    wFileNameLen  = (WORD) strlen (pszAttachmentName);
    wFileLabelLen = IsNullStr (pszLabelText) ? 0 : (WORD) strlen (pszLabelText);

    if (wFileLabelLen > 255)
        wFileLabelLen = 255;

    /* A buffer that fits all records (the name twice, the label and the icon) */
    Buffer.resize ((size_t) ICON_RECORDS_SIZE + (size_t) wFileNameLen * 2 + wFileLabelLen);
    pCDBuffer      = &Buffer[0];
    pCDBufferStart = pCDBuffer;

    /* HotSpotBegin */
    CDHotSpotBegin.Type  = HOTSPOTREC_TYPE_FILE;
    CDHotSpotBegin.Flags = HOTSPOTREC_RUNFLAG_BEGIN | HOTSPOTREC_RUNFLAG_NOBORDER;

    /* The attachment name is added twice (assigned name for linking, original file name) */
    CDHotSpotBegin.DataLength       = (WORD) ((wFileNameLen * 2) + 2);
    CDHotSpotBegin.Header.Signature = SIG_CD_HOTSPOTBEGIN;
    CDHotSpotBegin.Header.Length    = (WORD) (ODSLength (_CDHOTSPOTBEGIN) + CDHotSpotBegin.DataLength);

    ODSWriteMemory ((void **) &pCDBuffer, _CDHOTSPOTBEGIN, &CDHotSpotBegin, 1);

    memcpy (pCDBuffer, pszAttachmentName, (size_t) wFileNameLen + 1);
    pCDBuffer += wFileNameLen + 1;
    memcpy (pCDBuffer, pszAttachmentName, (size_t) wFileNameLen + 1);
    pCDBuffer += wFileNameLen + 1;

    if (CDHotSpotBegin.Header.Length % 2)
        *pCDBuffer++ = '\0';

    /* Graphic */
    CdGraphic.Header.Signature = SIG_CD_GRAPHIC;
    CdGraphic.Header.Length    = (WORD) ODSLength (_CDGRAPHIC);
    CdGraphic.Version          = CDGRAPHIC_VERSION2;

    WriteRecord (&pCDBuffer, _CDGRAPHIC, &CdGraphic, CdGraphic.Header.Length);

    /* Bitmap header */
    CdBitMapHeader.Header.Signature = SIG_CD_BITMAPHEADER;
    CdBitMapHeader.Header.Length    = (WORD) ODSLength (_CDBITMAPHEADER);
    CdBitMapHeader.Width            = BM_HDR_WH;
    CdBitMapHeader.Height           = BM_HDR_WH;
    CdBitMapHeader.BitsPerPixel     = BM_HDR_BP;
    CdBitMapHeader.SamplesPerPixel  = BM_HDR_SP;
    CdBitMapHeader.BitsPerSample    = BM_HDR_BP;
    CdBitMapHeader.SegmentCount     = BM_HDR_SEG;
    CdBitMapHeader.ColorCount       = BM_HDR_CC;
    CdBitMapHeader.PatternCount     = BM_HDR_PC;

    WriteRecord (&pCDBuffer, _CDBITMAPHEADER, &CdBitMapHeader, CdBitMapHeader.Header.Length);

    /* Bitmap segment, followed by the bitmap data */
    CdBitMapSegment.Header.Signature = SIG_CD_BITMAPSEGMENT;
    CdBitMapSegment.ScanlineCount    = BM_SEG_SL;
    CdBitMapSegment.DataSize         = (WORD) sizeof (g_BitmapSegment);
    CdBitMapSegment.Header.Length    = (WORD) (ODSLength (_CDBITMAPSEGMENT) + CdBitMapSegment.DataSize);

    ODSWriteMemory ((void **) &pCDBuffer, _CDBITMAPSEGMENT, &CdBitMapSegment, 1);

    memcpy (pCDBuffer, g_BitmapSegment, CdBitMapSegment.DataSize);
    pCDBuffer += CdBitMapSegment.DataSize;

    if (CdBitMapSegment.Header.Length % 2)
        *pCDBuffer++ = '\0';

    /* Color table, followed by its entries */
    CdColorTable.Header.Signature = SIG_CD_COLORTABLE;
    CdColorTable.Header.Length    = (WORD) (ODSLength (_CDCOLORTABLE) + sizeof (g_ColorTableEntries));

    ODSWriteMemory ((void **) &pCDBuffer, _CDCOLORTABLE, &CdColorTable, 1);

    memcpy (pCDBuffer, g_ColorTableEntries, sizeof (g_ColorTableEntries));
    pCDBuffer += sizeof (g_ColorTableEntries);

    if (sizeof (g_ColorTableEntries) % 2)
        *pCDBuffer++ = '\0';

    /* Pattern table, followed by its entries */
    CdPatternTable.Header.Signature = SIG_CD_PATTERNTABLE;
    CdPatternTable.Header.Length    = (WORD) (ODSLength (_CDPATTERNTABLE) + sizeof (g_PatternTableEntries));

    ODSWriteMemory ((void **) &pCDBuffer, _CDPATTERNTABLE, &CdPatternTable, 1);

    memcpy (pCDBuffer, g_PatternTableEntries, sizeof (g_PatternTableEntries));
    pCDBuffer += sizeof (g_PatternTableEntries);

    if (CdPatternTable.Header.Length % 2)
        *pCDBuffer++ = '\0';

    /* Text (in any case a text record, even if empty) */
    CDText.Header.Signature = SIG_CD_TEXT;
    CDText.Header.Length    = (WORD) (ODSLength (_CDTEXT) + wFileLabelLen);
    CDText.FontID           = DEFAULT_FONT_ID;

    ODSWriteMemory ((void **) &pCDBuffer, _CDTEXT, &CDText, 1);

    if (wFileLabelLen)
    {
        memcpy (pCDBuffer, pszLabelText, wFileLabelLen);
        pCDBuffer += wFileLabelLen;
    }

    if (CDText.Header.Length % 2)
        *pCDBuffer++ = '\0';

    /* HotSpotEnd */
    CDHotSpotEnd.Header.Length    = (BYTE) ODSLength (_CDHOTSPOTEND);
    CDHotSpotEnd.Header.Signature = SIG_CD_HOTSPOTEND;

    WriteRecord (&pCDBuffer, _CDHOTSPOTEND, &CDHotSpotEnd, CDHotSpotEnd.Header.Length);

    error = CompoundTextAddCDRecords (hCompound, pCDBufferStart, (DWORD) (pCDBuffer - pCDBufferStart));

    if (error)
        Err = "CompoundTextAddCDRecords (attachment hotspot) failed: " + ErrorText (error);

Done:

    return error;
}
