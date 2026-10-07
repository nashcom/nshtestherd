/* dominoload.h - the operations domlem performs against a Domino server (implemented in dominoload.cpp)
 *
 * DominoLoad owns the Notes handles: Init() opens the test database, Term() (or the destructor) releases everything.
 * Every test is a method, Op<Name>(). It does one operation and returns the Notes STATUS; Message has the result for
 * the coordinator (or the error text when it fails). New tests are added one after another as new methods.
 *
 * The handles belong to the identity the process has when Init() runs: with an identity switch (-switch), call
 * Init() after the switch.
 *
 *   DominoLoad Load;
 *   std::string Message;
 *
 *   Load.SetAgent ("TestAgent", 600);
 *   Load.Init ("server", "nshtestherd.nsf", Message);
 *   Load.OpRunAgent (Message);          // as often as needed
 *   Load.Term();
 */

#ifndef DOMLEM_DOMINOLOAD_H
#define DOMLEM_DOMINOLOAD_H

#include <string>
#include <vector>

#include <global.h>
#include <nsfdb.h>

#include "agentrun.h"
#include "mailclient.h"

/* What a mail of the mail job contains */
struct MailSettings
{
    MailSettings() : BodyKB (1, 20), BodyStyle (TEXT_LOREM), AttachCount (0, 2), AttachKB (10, 200), bAttachBinary (TRUE), dwRandomCount (0), DirectoryFile ("names.nsf") {}

    ValueRange BodyKB;           /* size of the generated body text in KB, fixed or a range, 0: no body */
    TextStyle BodyStyle;         /* kind of the body text */
    ValueRange AttachCount;      /* attachments per mail, fixed or a range */
    ValueRange AttachKB;         /* size of one attachment in KB, fixed or a range */
    BOOL      bAttachBinary;     /* random bytes (TRUE) or generated text (FALSE) */
    DWORD     dwRandomCount;     /* random recipients per mail, picked from the directory; 0: none */
    std::string RandomFilter;    /* selection formula of the people the random recipients are picked from (required) */
    std::string DirectoryFile;   /* the Domino Directory to search */
};


class DominoLoad
{
public:

    DominoLoad();
    ~DominoLoad();

    /* Settings: take effect with the next Init() */
    void SetAgent (const char *pszAgentName, DWORD dwTimeLimitSeconds);

    /* Mail: recipients of the messages, comma separated (empty: the current user, mail to self), and the content */
    void SetMail (const char *pszRecipients, const MailSettings &Settings);

    /* Opens the test database (pszServer may be empty: local; pszDbFile may be empty: no database, for jobs that need
     * none, like the mail job). On an error Err has the failing call with its error text
     * and nothing stays open. Calling it again closes what was open before and resets the counters. */
    STATUS Init (const char *pszServer, const char *pszDbFile, std::string &Err);

    /* Releases everything. Safe to call more than once. */
    void Term();

    BOOL IsInit() const { return m_bInit; }

    /* ---- the tests ---- */

    /* Opens the database as a new connection, reads the access level and closes it again (the test of a connect) */
    STATUS OpDbOpen (std::string &Message);

    /* Runs the agent once. The agent is opened with the first run and stays open until Term() */
    STATUS OpRunAgent (std::string &Message);

    /* Crafts one message and writes it into the mail.box of the server. The mail.box is opened with the first send */
    STATUS OpSendMail (std::string &Message);

private:

    STATUS LoadRandomRecipients (std::string &Err);

    DominoLoad (const DominoLoad &);                /* not copyable: owns Notes handles */
    DominoLoad &operator= (const DominoLoad &);

    BOOL        m_bInit;                    /* Init() succeeded */
    DBHANDLE    m_hDb;                      /* the test database, NULLHANDLE when Init() got no database name */
    char        m_szFullPath[MAXPATH+1];    /* server!!path of the test database */
    std::string m_DbFile;                   /* the database name as given, for messages */

    std::string m_AgentName;
    DWORD       m_dwAgentTimeLimit;
    AgentRunner m_Agent;

    std::string m_Server;                   /* server of the test database and of the mail.box */
    std::string m_UserName;                 /* the identity of the process, the sender of the mail */
    std::string m_MailTo;
    MailSettings m_MailSettings;
    std::vector<std::string> m_RandomNames;     /* full names of the people matching the filter, loaded with the first mail */
    BOOL         m_bRandomLoaded;
    MailClient  m_Mail;

    DWORD       m_dwDbOpens;
    DWORD       m_dwAgentRuns;
    DWORD       m_dwMailsSent;
};

#endif /* DOMLEM_DOMINOLOAD_H */
