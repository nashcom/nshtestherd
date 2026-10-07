/* dominoload.cpp - the operations domlem performs against a Domino server. Interface: dominoload.h */

#include <stdio.h>
#include <string.h>
#include <random>

#include <global.h>
#include <addin.h>
#include <miscerr.h>
#include <nsfdb.h>
#include <nsfnote.h>
#include <idtable.h>
#include <osfile.h>
#include <kfm.h>

#include "lib.h"
#include "dominoload.h"


DominoLoad::DominoLoad() : m_bInit (FALSE), m_hDb (NULLHANDLE), m_dwAgentTimeLimit (0), m_bRandomLoaded (FALSE), m_dwDbOpens (0), m_dwAgentRuns (0), m_dwMailsSent (0)
{
    m_szFullPath[0] = '\0';
}


DominoLoad::~DominoLoad()
{
    Term();
}


void DominoLoad::SetAgent (const char *pszAgentName, DWORD dwTimeLimitSeconds)
{
    m_AgentName        = pszAgentName ? pszAgentName : "";
    m_dwAgentTimeLimit = dwTimeLimitSeconds;
}


void DominoLoad::SetMail (const char *pszRecipients, const MailSettings &Settings)
{
    m_MailTo       = pszRecipients ? pszRecipients : "";
    m_MailSettings = Settings;
}


STATUS DominoLoad::Init (const char *pszServer, const char *pszDbFile, std::string &Err)
{
    STATUS error = NOERROR;
    char   szUserName[MAXUSERNAME+1] = {0};

    Term();

    m_Server    = pszServer ? pszServer : "";
    m_DbFile    = pszDbFile ? pszDbFile : "";

    if (!m_DbFile.empty())
    {
        error = OSPathNetConstruct (NULL, NullIfEmpty ((char *) pszServer), pszDbFile, m_szFullPath);

        if (error)
        {
            Err = std::string ("Cannot construct path for ") + pszDbFile + ": " + ErrorText (error);
            m_szFullPath[0] = '\0';
            goto Done;
        }

        error = NSFDbOpen (m_szFullPath, &m_hDb);

        if (error)
        {
            Err = std::string ("Cannot open database ") + m_szFullPath + ": " + ErrorText (error);
            m_hDb = NULLHANDLE;
            goto Done;
        }
    }

    m_dwDbOpens   = 0;
    m_dwAgentRuns = 0;
    m_dwMailsSent = 0;

    /* The sender of the mail: the identity the process has now */
    error = SECKFMGetUserName (szUserName);

    if (error)
    {
        Err = "Cannot get the current user name: " + ErrorText (error);
        goto Done;
    }

    m_UserName = szUserName;
    m_bInit    = TRUE;

Done:

    /* Nothing stays open after an error */
    if (error)
        Term();

    return error;
}


void DominoLoad::Term()
{
    m_Agent.Close();            /* the agent uses the database handle: first */
    m_Mail.Close();
    m_bInit = FALSE;
    m_RandomNames.clear();
    m_bRandomLoaded = FALSE;

    if (m_hDb)
    {
        NSFDbClose (m_hDb);
        m_hDb = NULLHANDLE;
    }
}


STATUS DominoLoad::OpDbOpen (std::string &Message)
{
    STATUS   error           = NOERROR;
    DBHANDLE hDb             = NULLHANDLE;
    WORD     wAccessLevel    = 0;
    WORD     wAccessFlag     = 0;
    TIMEDATE DataModified    = {0};
    TIMEDATE NonDataModified = {0};
    char     szMessage[MAXPATH+64] = {0};

    if (!IsInit())
    {
        Message = "Not initialized";
        error   = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    if (NULLHANDLE == m_hDb)
    {
        Message = "No database for this job";
        error   = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    /* A new connection every time: the open of the owned handle would not exercise the connect */
    error = NSFDbOpenExtended (m_szFullPath, 0, NULLHANDLE, NULL, &hDb, &DataModified, &NonDataModified);

    if (error)
    {
        Message = std::string ("Cannot open database ") + m_szFullPath + ": " + ErrorText (error);
        hDb     = NULLHANDLE;
        goto Done;
    }

    NSFDbAccessGet (hDb, &wAccessLevel, &wAccessFlag);

    m_dwDbOpens++;

    snprintf (szMessage, sizeof (szMessage), "dbopen #%lu %s access level %d", (unsigned long) m_dwDbOpens, m_DbFile.c_str(), (int) wAccessLevel);
    Message = szMessage;

Done:

    /* NSFDbCloseSession closes the database handle itself: no NSFDbClose for it */
    if (hDb)
        NSFDbCloseSession (hDb);

    return error;
}


STATUS DominoLoad::OpRunAgent (std::string &Message)
{
    STATUS error = NOERROR;
    char   szMessage[MAXPATH+MAXUSERNAME+64] = {0};

    if (!IsInit())
    {
        Message = "Not initialized";
        error   = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    if (NULLHANDLE == m_hDb)
    {
        Message = "No database for this job";
        error   = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    if (!m_Agent.IsOpen())
    {
        m_Agent.SetTimeLimit (m_dwAgentTimeLimit);

        error = m_Agent.Open (m_hDb, m_AgentName.c_str(), Message);

        if (error)
            goto Done;
    }

    error = m_Agent.Run (Message);

    if (error)
        goto Done;

    m_dwAgentRuns++;

    snprintf (szMessage, sizeof (szMessage), "agent #%lu %s in %s completed", (unsigned long) m_dwAgentRuns, m_AgentName.c_str(), m_DbFile.c_str());
    Message = szMessage;

Done:

    return error;
}


/* The people random recipients are picked from: the full names of all documents of the directory that match the filter.
 * Done once with the first mail. A limit keeps the list in bounds for a very large directory. */

#define MAX_RANDOM_NAMES   20000

STATUS DominoLoad::LoadRandomRecipients (std::string &Err)
{
    STATUS     error     = NOERROR;
    DBHANDLE   hDir      = NULLHANDLE;
    DHANDLE    hTable    = NULLHANDLE;
    NOTEHANDLE hNote     = NULLHANDLE;
    DWORD      dwEntries = 0;
    DWORD      dwNoteID  = 0;
    BOOL       bMore     = FALSE;
    char       szPath[MAXPATH+1]       = {0};
    char       szName[MAXUSERNAME+1]   = {0};

    m_RandomNames.clear();

    if (m_MailSettings.RandomFilter.empty())
    {
        Err   = "Random recipients need a filter that selects the test users (-mailfilter)";
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    error = OSPathNetConstruct (NULL, NullIfEmpty ((char *) m_Server.c_str()), m_MailSettings.DirectoryFile.c_str(), szPath);

    if (error)
    {
        Err = "Cannot construct the path of the directory: " + ErrorText (error);
        goto Done;
    }

    error = NSFDbOpen (szPath, &hDir);

    if (error)
    {
        Err  = std::string ("Cannot open the directory ") + szPath + ": " + ErrorText (error);
        hDir = NULLHANDLE;
        goto Done;
    }

    error = GetDocsByFormula (hDir, m_MailSettings.RandomFilter.c_str(), NULL, &hTable, &dwEntries);

    if (error)
    {
        Err = "The filter for the random recipients failed: " + ErrorText (error);
        goto Done;
    }

    for (bMore = IDScan (hTable, TRUE, &dwNoteID); bMore && (m_RandomNames.size() < MAX_RANDOM_NAMES); bMore = IDScan (hTable, FALSE, &dwNoteID))
    {
        if (NOERROR != NSFNoteOpen (hDir, (NOTEID) dwNoteID, 0, &hNote))
        {
            hNote = NULLHANDLE;
            continue;
        }

        /* FullName is a list: the first value is the full hierarchical name */
        szName[0] = '\0';
        NSFItemGetText (hNote, "FullName", szName, (WORD) sizeofstring (szName));

        if (szName[0])
            m_RandomNames.push_back (szName);

        NSFNoteClose (hNote);
        hNote = NULLHANDLE;
    }

    if (m_RandomNames.empty())
    {
        Err   = "No person matches the filter for the random recipients: " + m_MailSettings.RandomFilter;
        error = ERR_NOT_FOUND;
        goto Done;
    }

    AddInLogMessageText ("%s: %lu recipients to pick from (filter: %s)", NOERROR, g_szLogPrefix, (unsigned long) m_RandomNames.size(), m_MailSettings.RandomFilter.c_str());
    m_bRandomLoaded = TRUE;

Done:

    if (hNote)
        NSFNoteClose (hNote);

    if (hTable)
        IDDestroyTable (hTable);

    if (hDir)
        NSFDbClose (hDir);

    return error;
}


STATUS DominoLoad::OpSendMail (std::string &Message)
{
    STATUS      error = NOERROR;
    std::vector<std::string> Recipients;
    size_t      nIndex = 0;
    char        szSubject[64]  = {0};
    char        szMessage[MAXUSERNAME+64] = {0};
    static std::mt19937 Random ((std::random_device()()));

    if (!IsInit())
    {
        Message = "Not initialized";
        error   = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    if (!m_Mail.IsOpen())
    {
        m_Mail.SetBodySize (ValueRange (m_MailSettings.BodyKB.dwMin * 1024, m_MailSettings.BodyKB.dwMax * 1024));
        m_Mail.SetBodyStyle (m_MailSettings.BodyStyle);
        m_Mail.SetAttachments (m_MailSettings.AttachCount, ValueRange (m_MailSettings.AttachKB.dwMin * 1024, m_MailSettings.AttachKB.dwMax * 1024), m_MailSettings.bAttachBinary);

        error = m_Mail.Open (m_Server.c_str(), Message);

        if (error)
            goto Done;
    }

    /* -mailto is a list; random recipients are added; without both the mail goes to the sender */
    Recipients = SplitList (m_MailTo.c_str(), ',');

    if (m_MailSettings.dwRandomCount)
    {
        if (!m_bRandomLoaded)
        {
            error = LoadRandomRecipients (Message);

            if (error)
                goto Done;
        }

        for (DWORD dwPick = 0; dwPick < m_MailSettings.dwRandomCount; dwPick++)
            Recipients.push_back (m_RandomNames[Random() % m_RandomNames.size()]);
    }

    if (Recipients.empty())
        Recipients.push_back (m_UserName);

    m_Mail.ClearRecipients();

    for (nIndex = 0; nIndex < Recipients.size(); nIndex++)
        m_Mail.AddSendTo (Recipients[nIndex].c_str());

    snprintf (szSubject, sizeof (szSubject), "domlem load test #%lu", (unsigned long) (m_dwMailsSent + 1));

    error = m_Mail.Send (m_UserName.c_str(), szSubject, Message);

    if (error)
        goto Done;

    m_dwMailsSent++;

    snprintf (szMessage, sizeof (szMessage), "mail #%lu submitted, %lu bytes text, %lu attachments with %lu bytes", (unsigned long) m_dwMailsSent, (unsigned long) m_Mail.GetLastBodyBytes(), (unsigned long) m_Mail.GetLastAttachCount(), (unsigned long) m_Mail.GetLastAttachBytes());
    Message = szMessage;

Done:

    return error;
}
