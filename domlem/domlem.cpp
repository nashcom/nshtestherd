/* domlem - Domino Performance Test Lemming: a worker for nshtestherd that does what the coordinator tells it
 *
 * domlem registers with the nshtestherd coordinator, receives a test account, and then only follows commands:
 * run <job> / pause / stop / idle. The protocol logic (registration, polling, acknowledgements, pause timer) is the
 * shared client core of nshtestherd (src/herdclient.*); this file supplies what is Notes specific:
 *
 *   - identity: keep the identity the process runs under (default), or with -switch make sure the user of the account
 *     exists (userreg.cpp), download its ID from the ID vault and switch to it
 *   - the jobs (one operation per poll): "dbopen" opens a database as that user and reads the access level, "agent" runs
 *     an agent, "mail" writes a message into the mail.box
 *   - waiting and logging through the Domino add-in services (a "quit" is noticed between two operations, not in
 *     the middle of one)
 *
 * One process is one client with one identity (the identity switch is process-wide). Start it many times for many
 * clients: it registers itself with a unique request key.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

#include <algorithm>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "herdclient.h"

#include <global.h>
#include <acl.h>
#include <addin.h>
#include <nsfdb.h>
#include <osfile.h>
#include <osmem.h>
#include <osenv.h>
#include <osmisc.h>
#include <kfm.h>
#include <miscerr.h>
#include <idvault.h>
#include <secerr.h>

#include "lib.h"
#include "dominoload.h"

#include "userreg.h"

#define DOMLEM_VERSION "0.1.0"

/* Job "agent:<name>" runs the agent <name> (name or alias) of -db instead of -agent */
#define JOB_AGENT_PREFIX "agent:"

/* The ID of a user registered just now: download attempts and the pause between them (about one minute in all) */
#define VAULT_ATTEMPTS   13
#define VAULT_RETRY_MS   5000


/* Globals (set from the command line) */

char g_szLogPrefix[255]        = "domlem";
char g_szServer[MAXUSERNAME+1] = {0};                        /* -server: Domino server to work against, default: this server */
char g_szCoordinatorURL[256]   = "http://127.0.0.1:8788";    /* -coordinator: nshtestherd */
char g_szDbFilePath[MAXPATH+1] = "nshtestherd.nsf";          /* -db: the test database with the agents of the agent job */
char g_szOpenDbFilePath[MAXPATH+1] = "names.nsf";            /* -dbopen: database the dbopen job opens (read only: open, access level, close) */
char g_szAgentName[MAXUSERNAME+1] = "TestAgent";                  /* -agent: agent run by the agent job (in the -db database) */
DWORD g_dwAgentTimeout        = 600;                         /* -agenttimeout: seconds per agent run (10 minutes), 0: no limit */
char g_szMailTo[1024]            = {0};                         /* -mailto: recipients of the mail job (comma separated), default: the current user (mail to self) */
MailSettings g_MailSettings;                                  /* -mailsize, -mailtext, -mailattach, -mailattachsize, -mailattachtype */
BOOL g_bSwitch                 = FALSE;                      /* -switch: use the account's own identity */
BOOL g_bSwitchSet              = FALSE;                      /* set on this server (command line or DOMLEM_SWITCH): the coordinator's worker options do not change it */
BOOL g_bCADerived              = FALSE;                      /* the Domino CA name comes from the server name, not from -ca */
BOOL g_bWaitForAccount           = TRUE;                       /* nowait turns it off: without a free account the task ends */
DWORD g_dwPollSeconds          = 2;                          /* -poll */

DomRegSetup g_RegSetup;                                       /* -ca, -certid, -template, -policy, -maildir (and DOMLEM_CERTPW) */
std::string g_Token;                                          /* NSHTEST_TOKEN or DOMLEM_TOKEN: the coordinator's token (never an option) */

/* Registration defaults, used when g_RegSetup does not say otherwise (declared in userreg.h) */
WORD g_wMailSystem             = 0;                          /* 0: Notes mail */
char g_szDefaultMailDir[MAXPATH+1] = "mail";                 /* directory of new mail files */
WORD g_wDefaultMonths          = 120;                        /* validity of new certificates */


/* Job parameters (implemented below, after the option parser) */

static void   SaveJobDefaults();
static STATUS ApplyJobParams (const std::string &Params, std::string &Err);


/* What domlem writes, and where. The only write into the Domino Directory is the registration of a test user
 * (REGNewPerson in userreg.cpp, only with -switch); lookups and the people search for random recipients only read.
 * Mail goes into mail.box and the sender's own mail file. Any new write path into the directory (for example test
 * groups) must be an explicit, documented option.
 *
 * The agent job is the one path to arbitrary writes: an agent runs unrestricted. So it never runs agents of a system
 * database, even if -db names one. The list of system databases is Domino's own: dominosystemdbs.ind in the data
 * directory, one path per line relative to the data directory ("names.nsf", "mtdata/mtstore.nsf", "mail1.box"). Without
 * that file a built-in list of the main ones is used. Always refused as well: every mail box (mail.box, mailN.box) and
 * the directory of -mailnab. The path checked is the Domino logical name of the opened database (NSFDbPathGet), the
 * same form as in the file. A check against mistakes and against a run command that picks a directory agent; it is no
 * sandbox for an agent of the test database. */

#define SYSTEM_DBS_FILE "dominosystemdbs.ind"

/* A logical database name in one form for comparing: lower case, "/" between directories, no leading "/" or "./".
 * "Mtdata\MTStore.nsf" and "mtdata/mtstore.nsf" are the same database. */

static std::string NormalizeDbName (const std::string &Name)
{
    std::string Norm = Name;

    std::replace (Norm.begin(), Norm.end(), '\\', '/');
    std::transform (Norm.begin(), Norm.end(), Norm.begin(), [](unsigned char c) { return (char) tolower (c); });

    while (!Norm.empty() && ('/' == Norm[0]))
        Norm.erase (0, 1);

    while (0 == Norm.compare (0, 2, "./"))
        Norm.erase (0, 2);

    return Norm;
}


/* pszLogicalName: the Domino logical name of the opened database (DominoLoad::GetDbLogicalPath), relative to the data
 * directory. An empty name counts as a system database: what cannot be named is not run. */

static BOOL IsSystemDatabase (const char *pszLogicalName)
{
    static const char *Fallback[] = { "names.nsf", "admin4.nsf", "adminq.nsf", "log.nsf", "events4.nsf", "catalog.nsf",
                                      "domcfg.nsf", "statrep.nsf", "ddm.nsf", "certlog.nsf", "certstore.nsf", "busytime.nsf",
                                      "cldbdir.nsf", "domlog.nsf", "idpcat.nsf", "inetlockout.nsf" };

    BOOL                     bSystem = FALSE;
    char                     szIndFile[MAXPATH+1] = {0};
    std::string              Name = NormalizeDbName (pszLogicalName ? pszLogicalName : "");
    std::string              File;
    std::string              Line;
    std::vector<std::string> SystemDbs;

    if (Name.empty())
    {
        bSystem = TRUE;
        goto Done;
    }

    File = Name.substr ((std::string::npos == Name.find_last_of ('/')) ? 0 : Name.find_last_of ('/') + 1);

    /* Domino's list of its system databases in the data directory; read on every check (a job start), it is small */
    OSGetDataDirectory (szIndFile);
    OSPathAddTrailingPathSeparator (szIndFile, MAXPATH);

    if (strlen (szIndFile) + sizeofstring (SYSTEM_DBS_FILE) < sizeof (szIndFile))
    {
        std::ifstream In (std::string (szIndFile) + SYSTEM_DBS_FILE);

        while (In && std::getline (In, Line))
        {
            Line = NormalizeDbName (Line.substr (0, Line.find_last_not_of (" \t\r") + 1));

            if (!Line.empty())
                SystemDbs.push_back (Line);
        }
    }

    if (SystemDbs.empty())
        SystemDbs.assign (Fallback, Fallback + sizeof (Fallback) / sizeof (Fallback[0]));

    for (size_t nDb = 0; nDb < SystemDbs.size(); nDb++)
    {
        if (Name == SystemDbs[nDb])
            bSystem = TRUE;
    }

    /* Every mail box: mail.box, mail1.box ... mailN.box */
    if ((File.size() >= 8) && (0 == File.compare (0, 4, "mail")) && (0 == File.compare (File.size() - 4, 4, ".box")))
        bSystem = TRUE;

    /* The directory the random recipients come from */
    if (Name == NormalizeDbName (g_MailSettings.DirectoryFile))
        bSystem = TRUE;

Done:

    return bSystem;
}


/* The people random recipients are picked from when no -mailfilter is given: the test pool of this lemming's own
 * account. The short name without its trailing digits is the pool prefix ("load000003" -> "load"); the filter selects
 * the person documents whose short name is that prefix followed by digits. So a load test never mails a real user. */

static BOOL BuildPoolFilter (const std::string &ShortName, std::string &Filter, std::string &Err)
{
    BOOL        bOK    = FALSE;
    std::string Prefix = ShortName;

    while (!Prefix.empty() && (Prefix.back() >= '0') && (Prefix.back() <= '9'))
        Prefix.pop_back();

    /* A prefix, followed by at least one digit, and only characters that are literal in a formula string and in @Matches */
    if (Prefix.empty() || (Prefix.size() == ShortName.size()) || !IsSafeName (Prefix.c_str(), 64))
    {
        Err = "Cannot derive the test user filter from the short name [" + ShortName + "]: give -mailfilter";
        goto Done;
    }

    Filter = "Form = \"Person\" & @Matches(ShortName; \"" + Prefix + "{0-9}+{0-9}\")";
    bOK    = TRUE;

Done:

    return bOK;
}


/* The coordinator's worker options (--worker-options, the same for every worker of the herd). domlem takes "switch"
 * ("switch", "switch=1", "switch=0") and ignores every other name: the options may be meant for other kinds of workers.
 * This server has the last word: a switch set here (command line or DOMLEM_SWITCH, also =0) is not changed. */

static void ApplyWorkerOptions (const std::string &Options)
{
    std::vector<OptionPair> Pairs = SplitUrlOptions (Options.c_str());

    for (size_t nPair = 0; nPair < Pairs.size(); nPair++)
    {
        const OptionPair &Pair    = Pairs[nPair];
        BOOL              bWanted = FALSE;

        if (0 != StrICmp (Pair.name.c_str(), "switch"))
            continue;

        bWanted = !Pair.bHasValue || (Pair.value != "0" && 0 != StrICmp (Pair.value.c_str(), "false") && 0 != StrICmp (Pair.value.c_str(), "no"));

        if (!g_bSwitchSet)
        {
            g_bSwitch = bWanted;
            AddInLogMessageText ("%s: Identity switch %s by the coordinator (worker options)", NOERROR, g_szLogPrefix, bWanted ? "on" : "off");
        }
        else if (bWanted != g_bSwitch)
            AddInLogMessageText ("%s: The coordinator asks for switch=%s: ignored, this server decides (command line or DOMLEM_SWITCH: switch=%s)",
                                 NOERROR, g_szLogPrefix, bWanted ? "1" : "0", g_bSwitch ? "1" : "0");
    }
}


/* The Notes side of a worker: how to wait, how to log, which identity to use and what a job does */

class DomlemHooks : public HerdHooks
{
public:

    DomlemHooks() : m_bStop (FALSE), m_Job (JOB_NONE)
    {
        m_szIDFile[0]   = '\0';
    }

    ~DomlemHooks()
    {
        RemoveIDFile();
    }

    /* ---- environment ---- */

    bool Stopping() override
    {
        return m_bStop || AddInShouldTerminate();
    }

    void Sleep (int nMilliseconds) override
    {
        /* AddInIdleDelay returns TRUE when the server asks the task to terminate */
        if (AddInIdleDelay ((DWORD) nMilliseconds))
            m_bStop = TRUE;
    }

    /* No account yet: the coordinator is not reachable, has no pool, or all accounts are taken. The task keeps asking. */
    void Waiting (const std::string &Reason) override
    {
        AddInSetStatusText ("Waiting for an account: %s", Reason.c_str());
    }

    void Log (const std::string &testId, const std::string &text) override
    {
        AddInLogMessageText ("%s: [%s] %s", NOERROR, g_szLogPrefix, testId.c_str(), text.c_str());
    }

    /* The status text in "show tasks" shows what the coordinator sees: "Idle: waiting for work", "Running: mail #12 ...",
     * "Idle: job mail failed: ..." */
    void StateChanged (const std::string &State, const std::string &Message) override
    {
        std::string Text = State;

        if (!Text.empty())
            Text[0] = (char) toupper ((unsigned char) Text[0]);

        AddInSetStatusText ("%s: %.80s", Text.c_str(), Message.c_str());
    }

    /* ---- identity ---- */

    bool Registered (const HerdAccount &Account, std::string &Err) override
    {
        STATUS error = NOERROR;
        char   szUserName[MAXUSERNAME+1] = {0};

        /* The coordinator's worker options may ask for the identity switch, unless this server decided already */
        ApplyWorkerOptions (Account.workerOptions);

        if (!g_bSwitch)
        {
            error = SECKFMGetUserName (szUserName);

            if (error)
            {
                Err = "Cannot get the current identity: " + ErrorText (error);
                goto Done;
            }

            AddInLogMessageText ("%s: Keeping the current identity [%s]", NOERROR, g_szLogPrefix, szUserName);
            goto Done;
        }

        error = SwitchToAccount (Account, Err);

Done:

        return (NOERROR == error);
    }

    /* ---- job ---- */

    bool JobStart (const HerdJobContext &Context, std::string &Message, std::string &Err) override
    {
        bool         bStarted  = false;
        bool         bMailTest = false;   /* "mailtest": the mail job with everything random */
        std::string  AgentName;           /* "agent:<name>": this agent instead of -agent */
        std::string  LogicalPath;         /* the Domino logical name of the agent's database */
        MailSettings Mail;

        m_Job = JOB_NONE;

        if ("dbopen" == Context.job)
            m_Job = JOB_DBOPEN;
        else if ("agent" == Context.job)
            m_Job = JOB_AGENT;
        else if (0 == Context.job.compare (0, sizeofstring (JOB_AGENT_PREFIX), JOB_AGENT_PREFIX))
        {
            m_Job     = JOB_AGENT;
            AgentName = Context.job.substr (sizeofstring (JOB_AGENT_PREFIX));

            if (AgentName.empty() || (AgentName.size() > MAXUSERNAME))
            {
                Err = "job " + Context.job + ": no usable agent name after " JOB_AGENT_PREFIX;
                goto Done;
            }
        }
        else if ("mail" == Context.job)
            m_Job = JOB_MAIL;
        else if ("mailtest" == Context.job)
        {
            m_Job     = JOB_MAIL;
            bMailTest = true;
        }
        else
        {
            Err = "unknown job: " + Context.job + " (known jobs: dbopen, agent, agent:<name>, mail, mailtest)";
            goto Done;
        }

        /* The settings of this job: the command line defaults, changed by the parameters that came with the run command */
        if (ApplyJobParams (Context.params, Err))
            goto Done;

        /* What the options leave open, the job decides: "mail" is predictable, "mailtest" is random */
        Mail = g_MailSettings;

        if (SUBJECT_DEFAULT == Mail.Subject)
            Mail.Subject = bMailTest ? SUBJECT_RANDOM : SUBJECT_FIXED;

        if (SENTCOPY_DEFAULT == Mail.SentCopy)
            Mail.SentCopy = bMailTest ? SENTCOPY_AUTO : SENTCOPY_OFF;

        if (bMailTest && !Mail.bRandomCountSet)
            Mail.RandomCount = ValueRange (1, 3);

        /* Random recipients only from the test pool, unless -mailfilter says which people */
        if ((JOB_MAIL == m_Job) && Mail.RandomCount.dwMax && Mail.RandomFilter.empty())
        {
            if (!BuildPoolFilter (Context.account.shortName, Mail.RandomFilter, Err))
                goto Done;
        }

        /* The agent named in the job, else -agent. Only agents of -db: the database stays what the command line says. */
        if (AgentName.empty())
            AgentName = g_szAgentName;

        /* The handles are opened now, with the identity the process has now (after a -switch) */
        m_Load.SetAgent (AgentName.c_str(), g_dwAgentTimeout);
        m_Load.SetMail (g_szMailTo, Mail);
        m_Load.SetWorker (Context.account.testId.c_str(), Context.account.shortName.c_str());

        /* Each job opens only its own database: dbopen -dbopen (read only), agent -db, the mail jobs none */
        if (m_Load.Init (g_szServer, (JOB_MAIL == m_Job) ? "" : ((JOB_DBOPEN == m_Job) ? g_szOpenDbFilePath : g_szDbFilePath), Err))
            goto Done;

        /* An agent may write anywhere in its database: never in the directory or another system database. Checked with
         * the Domino logical name of the database that was opened, so no physical path, link or other spelling of a
         * system database gets through. Opening it did nothing; the handles are closed again in Done. */
        if (JOB_AGENT == m_Job)
        {
            LogicalPath = m_Load.GetDbLogicalPath();

            if (LogicalPath.empty() || IsSystemDatabase (LogicalPath.c_str()))
            {
                Err = "The agent job does not run agents of the system database [" + (LogicalPath.empty() ? std::string (g_szDbFilePath) : LogicalPath) + "]: use a test database (-db)";
                goto Done;
            }
        }

        Message = "job " + Context.job;
        bStarted = true;

Done:

        /* A job that cannot start is reported as failed; the task stays and waits for the next command */
        if (!bStarted)
        {
            m_Load.Term();
            m_Job = JOB_NONE;
        }

        return bStarted;
    }

    /* One operation per poll. A failed operation ends the job: it is reported as failed, the task waits for the next
     * command. The client core does not call JobAbort() for a job that ended by itself, so the handles go here. */
    HerdJobResult JobStep (std::string &Message) override
    {
        STATUS error = NOERROR;

        switch (m_Job)
        {
            case JOB_DBOPEN:
                error = m_Load.OpDbOpen (Message);
                break;

            case JOB_AGENT:
                error = m_Load.OpRunAgent (Message);
                break;

            case JOB_MAIL:
                error = m_Load.OpSendMail (Message);
                break;

            default:
                Message = "No job";
                error   = ERR_MISC_INVALID_ARGS;
                break;
        }

        if (error)
        {
            m_Load.Term();
            m_Job = JOB_NONE;
        }

        return error ? HERD_JOB_FAILED : HERD_JOB_RUNNING;
    }

    void JobAbort() override
    {
        m_Load.Term();
        m_Job = JOB_NONE;
    }

private:

    /* Mode -switch: make sure the user exists, download the ID from the vault, switch the process to it */
    STATUS SwitchToAccount (const HerdAccount &Account, std::string &Err)
    {
        STATUS error = NOERROR;
        char   szUserName[MAXUSERNAME+1]    = {0};
        char   szCurrentUser[MAXUSERNAME+1] = {0};
        char   szDataDir[MAXPATH+1]         = {0};
        char   szPassword[256]              = {0};
        char   szVaultServer[MAXUSERNAME+1] = {0};
        char   szMailServer[MAXUSERNAME+1]  = {0};
        char   szMailFile[MAXPATH+1]        = {0};
        int    nLen                         = 0;
        int    nAttempt                     = 0;
        BOOL   bCreated                     = FALSE;

        strncpy (szPassword, Account.password.c_str(), sizeofstring (szPassword));

        if (!g_RegSetup.certifier.file.empty() && !g_RegSetup.certifier.password.empty())
            AddInLogMessageText ("%s: Certifier for new users: the ID file [%s]", NOERROR, g_szLogPrefix, g_RegSetup.certifier.file.c_str());
        else if (!g_RegSetup.certifier.caName.empty())
            AddInLogMessageText ("%s: Certifier for new users: the Domino CA [%s]%s", NOERROR, g_szLogPrefix, g_RegSetup.certifier.caName.c_str(),
                                 g_bCADerived ? " of the server's organization (-ca or -certid to choose another)" : "");

        error = RegEnsureUser (&g_RegSetup,
                               Account.firstName.c_str(), Account.lastName.c_str(), Account.password.c_str(),
                               Account.shortName.c_str(), Account.internetAddress.c_str(),
                               szUserName, (WORD) MAXUSERNAME, &bCreated);

        if (error)
        {
            Err = "Cannot find or register user for account [" + Account.shortName + "]: " + ErrorText (error);
            goto Done;
        }

        /* An existing user is taken as it is: say so when it has no mail file, the mail jobs depend on it */
        if (!bCreated && !LookupMailFile (g_szServer, szUserName, szMailServer, (WORD) sizeof (szMailServer), szMailFile, (WORD) sizeof (szMailFile)))
            AddInLogMessageText ("%s: Warning: [%s] has no mail file in the directory: mail to it is not delivered, no sent copies", NOERROR, g_szLogPrefix, szUserName);

        /* The test id becomes part of a file name: only plain names */
        if (!IsSafeName (Account.testId.c_str(), 32))
        {
            Err   = "The test id of the account is not usable as a file name part";
            error = ERR_MISC_INVALID_ARGS;
            goto Done;
        }

        /* A private ID file per client, removed when the process ends */
        OSGetDataDirectory (szDataDir);
        OSPathAddTrailingPathSeparator (szDataDir, MAXPATH);
        nLen = snprintf (m_szIDFile, sizeof (m_szIDFile), "%sdomlem_%s_%lu.id", szDataDir, Account.testId.c_str(), (unsigned long) DOMLEM_GETPID());

        if ((nLen < 0) || ((size_t) nLen >= sizeof (m_szIDFile)))
        {
            m_szIDFile[0] = '\0';
            Err   = "ID file path too long";
            error = ERR_MISC_INVALID_ARGS;
            goto Done;
        }

        /* A user registered just now: its ID may need a moment to reach the vault. Retried for a while, only then. */
        for (nAttempt = 1; ; nAttempt++)
        {
            /* The server buffer is in/out: it comes back with the server that holds the vault. A copy for every attempt,
             * so that the server the work is done on stays the one that was configured. */
            CopyStr (szVaultServer, sizeof (szVaultServer), g_szServer);

            error = SECidfGet (szUserName, szPassword, m_szIDFile, NULL, szVaultServer, 0, 0, NULL);

            if (!error || !bCreated || (nAttempt >= VAULT_ATTEMPTS))
                break;

            if (1 == nAttempt)
                AddInLogMessageText ("%s: The ID of the new user [%s] is not in the vault yet: retrying for up to %d seconds", NOERROR, g_szLogPrefix, szUserName, (VAULT_ATTEMPTS - 1) * VAULT_RETRY_MS / 1000);

            /* TRUE: the server wants the task to end */
            if (AddInIdleDelay (VAULT_RETRY_MS))
            {
                m_bStop = TRUE;
                break;
            }
        }

        if (error)
        {
            Err = std::string ("Cannot download the ID of [") + szUserName + "] from the ID vault on [" + g_szServer + "]: " + ErrorText (error);
            goto Done;
        }

        error = SECKFMSwitchToIDFile (m_szIDFile, szPassword, szCurrentUser, MAXUSERNAME, fKFM_switchid_DontSetEnvVar, NULL);

        if (error)
        {
            Err = std::string ("Cannot switch to the ID of [") + szUserName + "]: " + ErrorText (error);
            goto Done;
        }

        AddInLogMessageText ("%s: Switched to user [%s]", NOERROR, g_szLogPrefix, szUserName);

Done:

        /* The ID file of a failed switch is of no use, and the password does not stay on the stack */
        if (error)
            RemoveIDFile();

        memset (szPassword, 0, sizeof (szPassword));

        return error;
    }

    void RemoveIDFile()
    {
        if (m_szIDFile[0])
        {
            remove (m_szIDFile);
            m_szIDFile[0] = '\0';
        }
    }

    enum JobType { JOB_NONE, JOB_DBOPEN, JOB_AGENT, JOB_MAIL };

    BOOL        m_bStop;
    JobType     m_Job;
    DominoLoad  m_Load;
    char        m_szIDFile[MAXPATH+1];
};


/* A request key that is unique per process, also across machines: a retry of THIS process returns the same account */

std::string BuildRequestKey()
{
    std::random_device Random;
    char               szKey[96] = {0};

    snprintf (szKey, sizeof (szKey), "domlem-%lx-%lx-%08x", (unsigned long) time (NULL), DOMLEM_GETPID(), (unsigned) Random());
    return szKey;
}


void Usage()
{
    AddInLogMessageText ("%s: Usage: load domlem [-coordinator <url>] [-server <domino server>] [-switch] [-dbopen <path>] [-db <path>] [-agent <name>] [-agenttimeout <seconds>] [-mailto <names>] [-mailsize <KB>] [-mailrandom <count>] [-mailfilter <formula>] [-mailsubject <random|fixed>] [-mailsentcopy <yes|no|auto>] [-mailnab <file>] [-mailtext <lorem|funny>] [-mailattach <count>] [-mailattachsize <KB>] [-mailattachtype <binary|text>] [-poll <seconds>]", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s: Options can be given as -name value, or URL style without blanks: name=value&name&name=value (%%20 is a blank, %%26 is &, %%3D is =)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   Example: load domlem coordinator=http://host:8788&switch&mailsize=20-50", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -coordinator  nshtestherd URL (default %s)", NOERROR, g_szLogPrefix, g_szCoordinatorURL);
    AddInLogMessageText ("%s:   -server       Domino server to work against (default: this server)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -switch       use the identity of the allocated account: register user if needed, ID from vault, switch (default: DOMLEM_SWITCH, else off; switch=0 turns it off)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -dbopen       database the dbopen job opens, read only (default %s)", NOERROR, g_szLogPrefix, g_szOpenDbFilePath);
    AddInLogMessageText ("%s:   -db           test database with the agents of the agent job, never a system database (default %s)", NOERROR, g_szLogPrefix, g_szDbFilePath);
    AddInLogMessageText ("%s:   -agent        agent run by the agent job, in the -db database (default %s)", NOERROR, g_szLogPrefix, g_szAgentName);
    AddInLogMessageText ("%s:   -agenttimeout execution limit of one agent run in seconds (default %lu, 0: no limit)", NOERROR, g_szLogPrefix, (unsigned long) g_dwAgentTimeout);
    AddInLogMessageText ("%s:   -mailto       recipients of the mail job, comma separated (default: the current user)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -mailrandom   random recipients per mail from the directory, a number or a range (default: mail 0, mailtest 1-3)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -mailfilter   selection formula of the people to pick from (default: the test pool of the account)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -mailsubject  random or fixed (default: mail fixed, mailtest random); always tagged [domlem t<id> <user> #<n>]", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -mailsentcopy yes, no or auto: copy in the sender's mail file (default: mail no, mailtest auto)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -mailnab      the directory to search (default %s)", NOERROR, g_szLogPrefix, g_MailSettings.DirectoryFile.c_str());
    AddInLogMessageText ("%s:   -mailtext     kind of the mail body text: lorem (default) or funny", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -mailsize     size of the mail body in KB, a number or a range like 1-20 (default %s)", NOERROR, g_szLogPrefix, RangeText (g_MailSettings.BodyKB).c_str());
    AddInLogMessageText ("%s:   -mailattach   number of attachments per mail, a number or a range like 0-3 (default %s)", NOERROR, g_szLogPrefix, RangeText (g_MailSettings.AttachCount).c_str());
    AddInLogMessageText ("%s:   -mailattachsize  size of one attachment in KB, a number or a range like 10-500 (default %s)", NOERROR, g_szLogPrefix, RangeText (g_MailSettings.AttachKB).c_str());
    AddInLogMessageText ("%s:   -mailattachtype  content of the attachments: binary (random bytes, default) or text", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -nowait       do not wait for a free account: end when none is left (default: wait)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -poll         polling interval in seconds (default %lu)", NOERROR, g_szLogPrefix, (unsigned long) g_dwPollSeconds);
    AddInLogMessageText ("%s: Secrets are never options: NSHTEST_TOKEN (coordinator token; DOMLEM_TOKEN overrides it) and DOMLEM_CERTPW come from the environment, else notes.ini", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s: Registration of missing users (-switch): certifier ID with DOMLEM_CERTPW is used directly, else the Domino CA", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -ca           Domino CA to certify with (when no certifier ID with password)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -certid       certifier ID file (password: environment variable DOMLEM_CERTPW)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -template     mail template for the new mail files (default: server default)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -maildir      directory of the mail files (default mail)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -policy       explicit policy for new users, for example the one with the ID vault", NOERROR, g_szLogPrefix);
}


/* Where an option comes from. The command line may set everything; options that come with a job (later: from the
 * coordinator) may only set what belongs to the job, never the identity, the server or the certifier. */

enum OptionScope
{
    SCOPE_COMMANDLINE,
    SCOPE_JOB
};


/* Options without a value in the classic form ("-switch") */

static BOOL IsFlagOption (const char *pszName)
{
    return (0 == StrICmp (pszName, "switch")) || (0 == StrICmp (pszName, "nowait"));
}


/* The options a job may set (the parameters of a run command): what a job contains, never where it works or whom it
 * reaches. The database, the agent (the job name "agent:<name>" picks one of -db), the recipients and the directory
 * filter stay what the command line says, so a command over the network cannot point a lemming elsewhere. */

static BOOL IsJobOption (const char *pszName)
{
    static const char *Names[] = { "agenttimeout", "mailrandom", "mailsize", "mailtext", "mailattach", "mailattachsize",
                                   "mailattachtype", "mailsubject", "mailsentcopy" };

    for (size_t nName = 0; nName < sizeof (Names) / sizeof (Names[0]); nName++)
    {
        if (0 == StrICmp (pszName, Names[nName]))
            return TRUE;
    }

    return FALSE;
}


/* Text into a fixed buffer: too long is refused, not cut */

static BOOL SetText (char *pszDest, size_t DestSize, const std::string &Value)
{
    if (Value.empty() || (Value.size() >= DestSize))
        return FALSE;

    memcpy (pszDest, Value.c_str(), Value.size() + 1);
    return TRUE;
}


/* Applies one option: the one place that knows the names. Err has the reason when it is refused. */

static STATUS ApplyOption (const OptionPair &Option, OptionScope Scope, std::string &Err)
{
    STATUS      error  = NOERROR;
    BOOL        bKnown = TRUE;
    BOOL        bOK    = FALSE;
    const char *pszName  = Option.name.c_str();
    const std::string &Value = Option.value;
    BOOL        bValue = Option.bHasValue && !Value.empty();

    if ((SCOPE_JOB == Scope) && !IsJobOption (pszName))
    {
        Err   = "The option [" + Option.name + "] cannot be set for a job";
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    if (0 == StrICmp (pszName, "switch"))
    {
        /* "switch" or "switch=1"; "switch=0" turns it off */
        g_bSwitch    = !Option.bHasValue || (Value != "0" && 0 != StrICmp (Value.c_str(), "false") && 0 != StrICmp (Value.c_str(), "no"));
        g_bSwitchSet = TRUE;
        bOK          = TRUE;
    }
    else if (0 == StrICmp (pszName, "nowait"))
    {
        /* "nowait" or "nowait=1"; "nowait=0" keeps waiting */
        g_bWaitForAccount = Option.bHasValue && (Value == "0" || 0 == StrICmp (Value.c_str(), "false") || 0 == StrICmp (Value.c_str(), "no"));
        bOK = TRUE;
    }
    else if (0 == StrICmp (pszName, "coordinator"))
        bOK = SetText (g_szCoordinatorURL, sizeof (g_szCoordinatorURL), Value);
    else if (0 == StrICmp (pszName, "server"))
        bOK = SetText (g_szServer, sizeof (g_szServer), Value);
    else if (0 == StrICmp (pszName, "db"))
        bOK = SetText (g_szDbFilePath, sizeof (g_szDbFilePath), Value);
    else if (0 == StrICmp (pszName, "dbopen"))
        bOK = SetText (g_szOpenDbFilePath, sizeof (g_szOpenDbFilePath), Value);
    else if (0 == StrICmp (pszName, "agent"))
        bOK = SetText (g_szAgentName, sizeof (g_szAgentName), Value);
    else if (0 == StrICmp (pszName, "agenttimeout"))
        bOK = bValue && ParseUnsigned (Value.c_str(), 86400, &g_dwAgentTimeout);
    else if (0 == StrICmp (pszName, "mailto"))
        bOK = SetText (g_szMailTo, sizeof (g_szMailTo), Value);
    else if (0 == StrICmp (pszName, "mailrandom"))
    {
        bOK = bValue && ParseRange (Value.c_str(), 50, &g_MailSettings.RandomCount);

        if (bOK)
            g_MailSettings.bRandomCountSet = TRUE;
    }
    else if (0 == StrICmp (pszName, "mailsubject"))
    {
        bOK = bValue && ((0 == StrICmp (Value.c_str(), "random")) || (0 == StrICmp (Value.c_str(), "fixed")));

        if (bOK)
            g_MailSettings.Subject = (0 == StrICmp (Value.c_str(), "random")) ? SUBJECT_RANDOM : SUBJECT_FIXED;
    }
    else if (0 == StrICmp (pszName, "mailsentcopy"))
    {
        bOK = TRUE;

        if (!bValue)
            bOK = FALSE;
        else if ((0 == StrICmp (Value.c_str(), "yes")) || (Value == "1"))
            g_MailSettings.SentCopy = SENTCOPY_ON;
        else if ((0 == StrICmp (Value.c_str(), "no")) || (Value == "0"))
            g_MailSettings.SentCopy = SENTCOPY_OFF;
        else if (0 == StrICmp (Value.c_str(), "auto"))
            g_MailSettings.SentCopy = SENTCOPY_AUTO;
        else
            bOK = FALSE;
    }
    else if (0 == StrICmp (pszName, "mailfilter"))
    {
        g_MailSettings.RandomFilter = Value;
        bOK = bValue;
    }
    else if (0 == StrICmp (pszName, "mailnab"))
    {
        g_MailSettings.DirectoryFile = Value;
        bOK = bValue;
    }
    else if (0 == StrICmp (pszName, "mailtext"))
        bOK = bValue && TextStyleFromName (Value.c_str(), g_MailSettings.BodyStyle);
    else if (0 == StrICmp (pszName, "mailsize"))
        bOK = bValue && ParseRange (Value.c_str(), 10240, &g_MailSettings.BodyKB);
    else if (0 == StrICmp (pszName, "mailattach"))
        bOK = bValue && ParseRange (Value.c_str(), 20, &g_MailSettings.AttachCount);
    else if (0 == StrICmp (pszName, "mailattachsize"))
        bOK = bValue && ParseRange (Value.c_str(), 102400, &g_MailSettings.AttachKB);
    else if (0 == StrICmp (pszName, "mailattachtype"))
    {
        bOK = bValue && (0 == StrICmp (Value.c_str(), "binary") || 0 == StrICmp (Value.c_str(), "text"));

        if (bOK)
            g_MailSettings.bAttachBinary = (0 == StrICmp (Value.c_str(), "binary"));
    }
    else if (0 == StrICmp (pszName, "poll"))
        bOK = bValue && ParseUnsigned (Value.c_str(), 3600, &g_dwPollSeconds) && (g_dwPollSeconds > 0);
    else if (0 == StrICmp (pszName, "ca"))
    {
        g_RegSetup.certifier.caName = Value;
        bOK = bValue;
    }
    else if (0 == StrICmp (pszName, "certid"))
    {
        g_RegSetup.certifier.file = Value;
        bOK = bValue;
    }
    else if (0 == StrICmp (pszName, "template"))
    {
        g_RegSetup.mail.templateFile = Value;
        bOK = bValue;
    }
    else if (0 == StrICmp (pszName, "maildir"))
    {
        g_RegSetup.mail.directory = Value;
        bOK = bValue;
    }
    else if (0 == StrICmp (pszName, "policy"))
    {
        g_RegSetup.policy = Value;
        bOK = bValue;
    }
    else
        bKnown = FALSE;

    if (!bKnown)
        Err = "Unknown option [" + Option.name + "]";
    else if (!bOK)
        Err = "Invalid or missing value for the option [" + Option.name + "]";

    error = bOK ? NOERROR : ERR_MISC_INVALID_ARGS;

Done:

    return error;
}


/* The job level settings as the command line left them. Every job starts from these, so that the parameters of one run
 * command do not stay in effect for the next one. */

struct JobDefaults
{
    char         szDbFilePath[MAXPATH+1];
    char         szAgentName[MAXUSERNAME+1];
    DWORD        dwAgentTimeout;
    char         szMailTo[1024];
    MailSettings Mail;
};

static JobDefaults g_JobDefaults;
static BOOL        g_bJobDefaultsSaved = FALSE;


static void SaveJobDefaults()
{
    memcpy (g_JobDefaults.szDbFilePath, g_szDbFilePath, sizeof (g_szDbFilePath));
    memcpy (g_JobDefaults.szAgentName, g_szAgentName, sizeof (g_szAgentName));
    memcpy (g_JobDefaults.szMailTo, g_szMailTo, sizeof (g_szMailTo));
    g_JobDefaults.dwAgentTimeout = g_dwAgentTimeout;
    g_JobDefaults.Mail           = g_MailSettings;
    g_bJobDefaultsSaved          = TRUE;
}


/* Back to the defaults, then the parameters of the run command ("name=value&name", the same names as the command line
 * options, but only the ones a job may set). A refused parameter makes the job fail with the reason. */

static STATUS ApplyJobParams (const std::string &Params, std::string &Err)
{
    STATUS                  error   = NOERROR;
    std::vector<OptionPair> Options = SplitUrlOptions (Params.c_str());

    if (g_bJobDefaultsSaved)
    {
        memcpy (g_szDbFilePath, g_JobDefaults.szDbFilePath, sizeof (g_szDbFilePath));
        memcpy (g_szAgentName, g_JobDefaults.szAgentName, sizeof (g_szAgentName));
        memcpy (g_szMailTo, g_JobDefaults.szMailTo, sizeof (g_szMailTo));
        g_dwAgentTimeout = g_JobDefaults.dwAgentTimeout;
        g_MailSettings   = g_JobDefaults.Mail;
    }

    for (size_t nOption = 0; nOption < Options.size(); nOption++)
    {
        error = ApplyOption (Options[nOption], SCOPE_JOB, Err);

        if (error)
            goto Done;
    }

Done:

    return error;
}


/* The command line in both forms, mixed as you like:
 *   classic:    -mailsize 20-50 -switch
 *   URL style:  mailsize=20-50&switch          (no blanks; %20, %26, %3D for a blank, & and = inside a value)
 * Both end up as name/value pairs that go through ApplyOption(). */

STATUS ParseCommandLine (int argc, char *argv[])
{
    STATUS      error = NOERROR;
    std::string Err;
    std::vector<OptionPair> Options;

    for (int i = 1; i < argc; i++)
    {
        const char *pszArg = argv[i];

        Options.clear();

        if ('-' == pszArg[0])
        {
            OptionPair Option;

            Option.name = (('-' == pszArg[1]) ? pszArg + 2 : pszArg + 1);

            if (!IsFlagOption (Option.name.c_str()) && (i + 1 < argc))
            {
                Option.value     = argv[++i];
                Option.bHasValue = TRUE;
            }

            Options.push_back (Option);
        }
        else
        {
            Options = SplitUrlOptions (pszArg);
        }

        for (size_t nOption = 0; nOption < Options.size(); nOption++)
        {
            error = ApplyOption (Options[nOption], SCOPE_COMMANDLINE, Err);

            if (error)
            {
                AddInLogMessageText ("%s: %s", NOERROR, g_szLogPrefix, Err.c_str());
                Usage();
                goto Done;
            }
        }
    }

    /* The jobs start from what the command line says */
    if (!error)
        SaveJobDefaults();

Done:

    return error;
}


/* A setting from outside the command line: a secret (a password, the token), which is never an option because it would
 * show up in the process list and in "show tasks", or a default for every lemming of the server (the coordinator).
 * First the environment of the server process (set it before the server starts, for example in the container), then
 * notes.ini (set config; readable there in plain text for anybody who can read notes.ini or run "show config"). */

static BOOL GetSetting (const char *pszName, std::string &Value)
{
    BOOL        bFound    = FALSE;
    const char *pszValue  = getenv (pszName);
    char        szValue[256] = {0};

    Value.clear();

    if (!IsNullStr (pszValue))
    {
        Value  = pszValue;
        bFound = TRUE;
    }
    else if (OSGetEnvironmentString (pszName, szValue, (WORD) sizeofstring (szValue)) && szValue[0])
    {
        Value  = szValue;
        bFound = TRUE;
    }

    memset (szValue, 0, sizeof (szValue));
    return bFound;
}


STATUS LNPUBLIC AddInMain (HMODULE hResourceModule, int argc, char far *argv[])
{
    STATUS error = NOERROR;

    AddInSetStatusText ("Starting");

    /* Defaults for every lemming of this server, so that "load domlem" needs no option; the command line overrides them.
     * The identity switch is set here or on the command line, never by the coordinator: it writes into the directory. */
    {
        std::string Value;

        if (GetSetting ("DOMLEM_COORDINATOR", Value) && !SetText (g_szCoordinatorURL, sizeof (g_szCoordinatorURL), Value))
            AddInLogMessageText ("%s: DOMLEM_COORDINATOR is too long: ignored", NOERROR, g_szLogPrefix);

        if (GetSetting ("DOMLEM_SWITCH", Value))
        {
            if ((Value == "1") || (0 == StrICmp (Value.c_str(), "yes")) || (0 == StrICmp (Value.c_str(), "true")))
                g_bSwitch = g_bSwitchSet = TRUE;
            else if ((Value == "0") || (0 == StrICmp (Value.c_str(), "no")) || (0 == StrICmp (Value.c_str(), "false")))
            {
                g_bSwitch    = FALSE;
                g_bSwitchSet = TRUE;
            }
            else
                AddInLogMessageText ("%s: DOMLEM_SWITCH=%s is not 1/yes/true or 0/no/false: ignored", NOERROR, g_szLogPrefix, Value.c_str());
        }
    }

    error = ParseCommandLine (argc, argv);

    if (error)
        goto Done;

    /* No -server: work against the server this task runs on (its own name is the name of the ID it runs under) */
    if (IsNullStr (g_szServer))
    {
        error = SECKFMGetUserName (g_szServer);

        if (error)
        {
            AddInLogMessageText ("%s: Error getting current username", error, g_szLogPrefix);
            goto Done;
        }
    }

    /* No certifier given: the Domino CA of the server's own organization, "CN=srv/OU=x/O=Org" gives "/OU=x/O=Org". Set
     * also without -switch: the coordinator's worker options may still ask for it. Logged when it is used. */
    if (g_RegSetup.certifier.caName.empty() && g_RegSetup.certifier.file.empty())
    {
        const char *pszOrg = strchr (g_szServer, '/');

        if ((0 == strncmp (g_szServer, "CN=", 3)) && pszOrg && pszOrg[1])
        {
            g_RegSetup.certifier.caName = pszOrg;
            g_bCADerived                = TRUE;
        }
    }

    /* ID files and attachment data of lemmings that crashed: removed, the files of running lemmings stay */
    RemoveStaleTempFiles();

    /* Registration happens on the Domino server we work against. The certifier password is not an option: it would
       show up in the process list and in "show tasks" */
    g_RegSetup.server = g_szServer;

    {
        std::string Secret;

        if (GetSetting ("DOMLEM_CERTPW", Secret))
            g_RegSetup.certifier.password = Secret;    /* kept only in the registration setup, needed for every new user */

        /* The coordinator's token: NSHTEST_TOKEN like every nshtestherd component, DOMLEM_TOKEN wins when both are set */
        if (GetSetting ("DOMLEM_TOKEN", Secret) || GetSetting ("NSHTEST_TOKEN", Secret))
            g_Token = Secret;

        std::fill (Secret.begin(), Secret.end(), '\0');
    }

    {
        DomlemHooks      Hooks;
        HerdClientConfig Config;
        HerdEnd          End = HERD_END_FAILED;

        Config.serverUrl  = g_szCoordinatorURL;
        Config.requestKey = BuildRequestKey();
        Config.name       = "domlem";
        Config.pollMs     = (int) (g_dwPollSeconds * 1000);
        Config.waitForAccount = g_bWaitForAccount;      /* default: more lemmings than accounts, the extra ones wait and show it */
        Config.httpTimeoutSeconds = 3;     /* a task must end promptly when the server shuts down */
        Config.token      = g_Token;

        AddInLogMessageText ("%s: %s, coordinator [%s]%s, Domino server [%s], identity: %s", NOERROR, g_szLogPrefix,
                             DOMLEM_VERSION, g_szCoordinatorURL, g_Token.empty() ? "" : " with token", g_szServer,
                             g_bSwitch ? "account (-switch)" : (g_bSwitchSet ? "current" : "current, unless the coordinator's worker options ask for switch"));

        HerdClient Client (Config, Hooks);
        End = Client.Run();

        if (HERD_END_CLEAN != End)
            error = ERR_MISC_INVALID_ARGS;
    }

Done:

    AddInSetStatusText ("Terminated");
    return error;
}
