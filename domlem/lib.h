/* lib.h - small helpers shared by the domlem modules (domlem.cpp, userreg.cpp) */

#ifndef DOMLEM_LIB_H
#define DOMLEM_LIB_H

#include <stddef.h>
#include <string>
#include <vector>

#include <global.h>
#include <nsfdb.h>

/* Process id, for names that must differ between the processes of one machine */
#ifdef UNIX
#include <unistd.h>
#define DOMLEM_GETPID() ((unsigned long) getpid())
#else
#include <process.h>
#define DOMLEM_GETPID() ((unsigned long) _getpid())
#endif

/* Case insensitive string compare, 0 when equal (used for the command line options) */
#ifdef UNIX
#include <strings.h>
#define StrICmp(a, b) strcasecmp ((a), (b))
#else
#include <string.h>
#define StrICmp(a, b) _stricmp ((a), (b))
#endif

/* Size of a character array without the terminating zero: the length to pass to calls that add the terminator
 * themselves. Only for arrays, not for pointers. The Notes headers do not define it on all platforms. */
#ifndef sizeofstring
#define sizeofstring(x) (sizeof (x) - 1)
#endif

/* Prefix of every log line, defined in domlem.cpp, used by all modules */
extern char g_szLogPrefix[255];

/* TRUE for NULL and for an empty string */
BOOL IsNullStr (const char *pszStr);

/* NULL for NULL or an empty string, else the string itself (the REG structures want NULL for "not set") */
char *NullIfEmpty (char *pszStr);

/* Copies into a buffer of DestSize bytes. Always terminated; a NULL source gives an empty string.
 * A source that does not fit is cut and logged */
void CopyStr (char *pszDest, size_t DestSize, const char *pszSource);

/* The text of a Notes error code with the code in hex, for logs and for messages: "File does not exist (0x0104)" */
std::string ErrorText (STATUS error);

/* Is there a person document for this name, short name or internet address? Returns TRUE when found and, when the
 * document has a name, its full name (pszRetFullName may be NULL). Looks into the Domino Directory of pszServer. */
BOOL LookupUser (const char *pszServer, const char *pszKey, char *pszRetFullName, WORD wMaxFullName);

/* The mail file of a user from its person document: MailServer and MailFile. Returns TRUE when the document has a mail
 * file; pszRetMailServer may come back empty (the person document has no mail server). Looks into the Domino Directory
 * of pszServer. */
BOOL LookupMailFile (const char *pszServer, const char *pszUserName, char *pszRetMailServer, WORD wMaxMailServer, char *pszRetMailFile, WORD wMaxMailFile);

/* Searches the documents of a database with a selection formula and collects their note IDs in an ID table.
 * *phNoteIDTable is NULLHANDLE: a table is created and the caller destroys it with IDDestroyTable(); else the IDs are
 * added to the given table. pszViewTitle is the view title for formulas that need one, NULL: none.
 * pdwEntries (may be NULL) receives the number of IDs in the table. */
STATUS GetDocsByFormula (DBHANDLE hDb, const char *pszFormula, const char *pszViewTitle, DHANDLE *phNoteIDTable, DWORD *pdwEntries);

/* Picks one random document that matches the formula: its note ID in *pRetNoteID, the number of matches in
 * *pdwMatches (may be NULL). ERR_NOT_FOUND when no document matches. Built on GetDocsByFormula(). */
STATUS GetRandomDocByFormula (DBHANDLE hDb, const char *pszFormula, const char *pszViewTitle, NOTEID *pRetNoteID, DWORD *pdwMatches);

/* Removes the temporary files a crashed lemming left in the Notes data directory: "domlem_<test_id>_<pid>.id" (an ID
 * file) and "domlem_att_<pid>_<mail>_<n>.tmp" (attachment data), but only when the process <pid> no longer exists, so
 * the files of the other lemmings on the server stay. Linux only (elsewhere it does nothing). */
void RemoveStaleTempFiles();

/* Is this a safe single file name component: letters, digits, dot, dash, underscore; no path separators, no "..", at most
 * wMax characters? Used before a value from outside becomes part of a path. */
BOOL IsSafeName (const char *pszName, size_t nMaxLen);

/* Parses a plain decimal number without sign or trailing characters, range 0..dwMax. FALSE: not a number or too large. */
BOOL ParseUnsigned (const char *pszText, DWORD dwMax, DWORD *pdwValue);

/* A value that is either fixed (min = max) or varies between min and max */
struct ValueRange
{
    ValueRange (DWORD dwValue = 0) : dwMin (dwValue), dwMax (dwValue) {}
    ValueRange (DWORD dwFrom, DWORD dwTo) : dwMin (dwFrom), dwMax (dwTo) {}

    DWORD dwMin;
    DWORD dwMax;
};

/* Parses "N" (fixed) or "min-max" (plain decimal numbers, min <= max, max <= dwLimit). FALSE for anything else. */
BOOL ParseRange (const char *pszText, DWORD dwLimit, ValueRange *pRange);

/* A value in the range, the same for the same seed (a different seed gives another value) */
DWORD PickInRange (const ValueRange &Range, unsigned long ulSeed);

/* "N" or "min-max", for messages */
std::string RangeText (const ValueRange &Range);

/* One option as name and value. bHasValue is FALSE for a flag without value. */
struct OptionPair
{
    OptionPair() : bHasValue (FALSE) {}

    std::string name;
    std::string value;
    BOOL        bHasValue;
};

/* Decodes %XX escapes (%20 is a blank, %26 is &, %3D is =). Everything else is taken as it is. */
std::string PercentDecode (const char *pszText);

/* Splits URL style options without blanks, "name=value&name&name=value", into pairs. Names and values are decoded. */
std::vector<OptionPair> SplitUrlOptions (const char *pszText);

/* Splits a list like "a, b ,c" at the delimiter: the entries without the surrounding blanks, empty entries dropped */
std::vector<std::string> SplitList (const char *pszList, char chDelimiter);

/* Appends the attachment hotspot - the icon with a label text after it - for pszAttachmentName (the name the file was
 * attached with by NSFNoteAttachFile) to the open compound text hCompound. NSFNoteAttachFile() only stores the file in a
 * $FILE item; the hotspot makes it show up as an icon in the rich text body and lets LotusScript find it when looping
 * through the body. Records: CDHOTSPOTBEGIN (file), CDGRAPHIC with CDBITMAPHEADER, CDBITMAPSEGMENT, CDCOLORTABLE and
 * CDPATTERNTABLE, CDTEXT, CDHOTSPOTEND. On an error Err has the failing step. */
STATUS AddAttachmentHotspot (DHANDLE hCompound, const char *pszAttachmentName, const char *pszLabelText, std::string &Err);

#endif /* DOMLEM_LIB_H */
