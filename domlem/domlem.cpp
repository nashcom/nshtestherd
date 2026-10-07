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
#include <time.h>

#include <random>
#include <string>

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


/* Globals (set from the command line) */

char g_szLogPrefix[255]        = "domlem";
char g_szServer[MAXUSERNAME+1] = {0};                        /* -server: Domino server to work against, default: this server */
char g_szCoordinatorURL[256]   = "http://127.0.0.1:8788";    /* -coordinator: nshtestherd */
char g_szDbFilePath[MAXPATH+1] = "nshtestherd.nsf";          /* -db: database opened by the dbopen job */
char g_szAgentName[MAXUSERNAME+1] = "TestAgent";                  /* -agent: agent run by the agent job (in the -db database) */
DWORD g_dwAgentTimeout        = 600;                         /* -agenttimeout: seconds per agent run (10 minutes), 0: no limit */
char g_szMailTo[1024]            = {0};                         /* -mailto: recipients of the mail job (comma separated), default: the current user (mail to self) */
MailSettings g_MailSettings;                                  /* -mailsize, -mailtext, -mailattach, -mailattachsize, -mailattachtype */
BOOL g_bSwitch                 = FALSE;                      /* -switch: use the account's own identity */
BOOL g_bWaitForAccount           = TRUE;                       /* nowait turns it off: without a free account the task ends */
DWORD g_dwPollSeconds          = 2;                          /* -poll */

DomRegSetup g_RegSetup;                                       /* -ca, -certid, -template, -policy, -maildir (and DOMLEM_CERTPW) */

/* Registration defaults, used when g_RegSetup does not say otherwise (declared in userreg.h) */
WORD g_wMailSystem             = 0;                          /* 0: Notes mail */
char g_szDefaultMailDir[MAXPATH+1] = "mail";                 /* directory of new mail files */
WORD g_wDefaultMonths          = 120;                        /* validity of new certificates */


/* Job parameters (implemented below, after the option parser) */

static void   SaveJobDefaults();
static STATUS ApplyJobParams (const std::string &Params, std::string &Err);


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

    /* ---- identity ---- */

    bool Registered (const HerdAccount &Account, std::string &Err) override
    {
        STATUS error = NOERROR;
        char   szUserName[MAXUSERNAME+1] = {0};

        if (!g_bSwitch)
        {
            error = SECKFMGetUserName (szUserName);

            if (error)
            {
                Err = "Cannot get the current identity: " + ErrorText (error);
                goto Done;
            }

            AddInLogMessageText ("%s: Keeping the current identity [%s]", NOERROR, g_szLogPrefix, szUserName);
            AddInSetStatusText ("Registered, identity: %s", szUserName);
            goto Done;
        }

        error = SwitchToAccount (Account, Err);

        if (error)
            goto Done;

        AddInSetStatusText ("Registered, user: %s", Account.shortName.c_str());

Done:

        return (NOERROR == error);
    }

    /* ---- job ---- */

    bool JobStart (const HerdJobContext &Context, std::string &Message, std::string &Err) override
    {
        bool bStarted = false;

        m_Job = JOB_NONE;

        if ("dbopen" == Context.job)
            m_Job = JOB_DBOPEN;
        else if ("agent" == Context.job)
            m_Job = JOB_AGENT;
        else if ("mail" == Context.job)
            m_Job = JOB_MAIL;
        else
        {
            Err = "unknown job: " + Context.job + " (known jobs: dbopen, agent, mail)";
            goto Done;
        }

        /* The settings of this job: the command line defaults, changed by the parameters that came with the run command */
        if (ApplyJobParams (Context.params, Err))
            goto Done;

        /* The handles are opened now, with the identity the process has now (after a -switch) */
        m_Load.SetAgent (g_szAgentName, g_dwAgentTimeout);
        m_Load.SetMail (g_szMailTo, g_MailSettings);

        /* The mail job needs no database of the test: only the database jobs open -db */
        if (m_Load.Init (g_szServer, (JOB_MAIL == m_Job) ? "" : g_szDbFilePath, Err))
        {
            m_Job = JOB_NONE;
            goto Done;
        }

        Message = "job " + Context.job;

        AddInSetStatusText ("Running job %s", Context.job.c_str());
        bStarted = true;

Done:

        return bStarted;
    }

    /* One operation per poll */
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
        int    nLen                         = 0;

        strncpy (szPassword, Account.password.c_str(), sizeofstring (szPassword));

        error = RegEnsureUser (&g_RegSetup,
                               Account.firstName.c_str(), Account.lastName.c_str(), Account.password.c_str(),
                               Account.shortName.c_str(), Account.internetAddress.c_str(),
                               szUserName, (WORD) MAXUSERNAME);

        if (error)
        {
            Err = "Cannot find or register user for account [" + Account.shortName + "]: " + ErrorText (error);
            goto Done;
        }

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

        /* The server buffer is in/out: it comes back with the server that holds the vault. A copy, so that the server
         * the work is done on stays the one that was configured. */
        CopyStr (szVaultServer, sizeof (szVaultServer), g_szServer);

        error = SECidfGet (szUserName, szPassword, m_szIDFile, NULL, szVaultServer, 0, 0, NULL);

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
    AddInLogMessageText ("%s: Usage: load domlem [-coordinator <url>] [-server <domino server>] [-switch] [-db <path>] [-agent <name>] [-agenttimeout <seconds>] [-mailto <names>] [-mailsize <KB>] [-mailrandom <count>] [-mailfilter <formula>] [-mailnab <file>] [-mailtext <lorem|funny>] [-mailattach <count>] [-mailattachsize <KB>] [-mailattachtype <binary|text>] [-poll <seconds>]", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s: Options can be given as -name value, or URL style without blanks: name=value&name&name=value (%%20 is a blank, %%26 is &, %%3D is =)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   Example: load domlem coordinator=http://host:8788&switch&mailsize=20-50", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -coordinator  nshtestherd URL (default %s)", NOERROR, g_szLogPrefix, g_szCoordinatorURL);
    AddInLogMessageText ("%s:   -server       Domino server to work against (default: this server)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -switch       use the identity of the allocated account: register user if needed, ID from vault, switch", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -db           database for the dbopen job (default %s)", NOERROR, g_szLogPrefix, g_szDbFilePath);
    AddInLogMessageText ("%s:   -agent        agent run by the agent job, in the -db database (default %s)", NOERROR, g_szLogPrefix, g_szAgentName);
    AddInLogMessageText ("%s:   -agenttimeout execution limit of one agent run in seconds (default %lu, 0: no limit)", NOERROR, g_szLogPrefix, (unsigned long) g_dwAgentTimeout);
    AddInLogMessageText ("%s:   -mailto       recipients of the mail job, comma separated (default: the current user)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -mailrandom   number of random recipients per mail, picked from the directory (default 0)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -mailfilter   selection formula of the people to pick from (required with -mailrandom)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -mailnab      the directory to search (default %s)", NOERROR, g_szLogPrefix, g_MailSettings.DirectoryFile.c_str());
    AddInLogMessageText ("%s:   -mailtext     kind of the mail body text: lorem (default) or funny", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -mailsize     size of the mail body in KB, a number or a range like 1-20 (default %s)", NOERROR, g_szLogPrefix, RangeText (g_MailSettings.BodyKB).c_str());
    AddInLogMessageText ("%s:   -mailattach   number of attachments per mail, a number or a range like 0-3 (default %s)", NOERROR, g_szLogPrefix, RangeText (g_MailSettings.AttachCount).c_str());
    AddInLogMessageText ("%s:   -mailattachsize  size of one attachment in KB, a number or a range like 10-500 (default %s)", NOERROR, g_szLogPrefix, RangeText (g_MailSettings.AttachKB).c_str());
    AddInLogMessageText ("%s:   -mailattachtype  content of the attachments: binary (random bytes, default) or text", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -nowait       do not wait for a free account: end when none is left (default: wait)", NOERROR, g_szLogPrefix);
    AddInLogMessageText ("%s:   -poll         polling interval in seconds (default %lu)", NOERROR, g_szLogPrefix, (unsigned long) g_dwPollSeconds);
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


/* The options a job may set */

static BOOL IsJobOption (const char *pszName)
{
    static const char *Names[] = { "db", "agent", "agenttimeout", "mailto", "mailrandom", "mailfilter", "mailnab",
                                   "mailsize", "mailtext", "mailattach", "mailattachsize", "mailattachtype" };

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
        g_bSwitch = !Option.bHasValue || (Value != "0" && 0 != StrICmp (Value.c_str(), "false") && 0 != StrICmp (Value.c_str(), "no"));
        bOK       = TRUE;
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
    else if (0 == StrICmp (pszName, "agent"))
        bOK = SetText (g_szAgentName, sizeof (g_szAgentName), Value);
    else if (0 == StrICmp (pszName, "agenttimeout"))
        bOK = bValue && ParseUnsigned (Value.c_str(), 86400, &g_dwAgentTimeout);
    else if (0 == StrICmp (pszName, "mailto"))
        bOK = SetText (g_szMailTo, sizeof (g_szMailTo), Value);
    else if (0 == StrICmp (pszName, "mailrandom"))
        bOK = bValue && ParseUnsigned (Value.c_str(), 50, &g_MailSettings.dwRandomCount);
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

    if (g_MailSettings.dwRandomCount && g_MailSettings.RandomFilter.empty())
    {
        Err   = "mailrandom needs mailfilter, a formula that selects the test users only";
        error = ERR_MISC_INVALID_ARGS;
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

    /* Random recipients without a filter could mail real users: refuse to start */
    if (!error && g_MailSettings.dwRandomCount && g_MailSettings.RandomFilter.empty())
    {
        AddInLogMessageText ("%s: -mailrandom needs -mailfilter, a formula that selects the test users only", NOERROR, g_szLogPrefix);
        error = ERR_MISC_INVALID_ARGS;
    }

    /* The jobs start from what the command line says */
    if (!error)
        SaveJobDefaults();

Done:

    return error;
}


STATUS LNPUBLIC AddInMain (HMODULE hResourceModule, int argc, char far *argv[])
{
    STATUS error = NOERROR;

    AddInSetStatusText ("Starting");

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

    /* Registration happens on the Domino server we work against. The certifier password is not an option: it would
       show up in the process list and in "show tasks" */
    g_RegSetup.server = g_szServer;

    {
        char szCertPassword[256] = {0};

        if (OSGetEnvironmentString ("DOMLEM_CERTPW", szCertPassword, (WORD) sizeofstring (szCertPassword)))
            g_RegSetup.certifier.password = szCertPassword;
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

        AddInLogMessageText ("%s: %s, coordinator [%s], Domino server [%s], identity: %s", NOERROR, g_szLogPrefix,
                             DOMLEM_VERSION, g_szCoordinatorURL, g_szServer, g_bSwitch ? "account (-switch)" : "current");

        HerdClient Client (Config, Hooks);
        End = Client.Run();

        if (HERD_END_CLEAN != End)
            error = ERR_MISC_INVALID_ARGS;
    }

Done:

    AddInSetStatusText ("Terminated");
    return error;
}
