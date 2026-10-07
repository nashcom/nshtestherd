/* agentrun.cpp - run an agent of a Domino database. Interface: agentrun.h */

#include <stdio.h>
#include <string.h>

#include <global.h>
#include <miscerr.h>
#include <nsfdb.h>
#include <nsfnote.h>
#include <nif.h>
#include <agents.h>

#include "lib.h"
#include "agentrun.h"


AgentRunner::AgentRunner() : m_dwTimeLimit (0), m_bOwnsDb (FALSE), m_hDb (NULLHANDLE), m_hAgent (NULL)
{
}


AgentRunner::~AgentRunner()
{
    Close();
}


void AgentRunner::SetTimeLimit (DWORD dwSeconds)
{
    m_dwTimeLimit = dwSeconds;
}


STATUS AgentRunner::Open (const char *pszDbPath, const char *pszAgentName, std::string &Err)
{
    STATUS error = NOERROR;

    Close();

    if (IsNullStr (pszDbPath) || IsNullStr (pszAgentName))
    {
        Err   = "No database or agent name";
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    error = NSFDbOpen (pszDbPath, &m_hDb);

    if (error)
    {
        Err   = std::string ("NSFDbOpen (") + pszDbPath + ") failed: " + ErrorText (error);
        m_hDb = NULLHANDLE;
        goto Done;
    }

    m_bOwnsDb = TRUE;

    error = OpenAgent (pszAgentName, Err);

Done:

    if (error)
        Close();

    return error;
}


STATUS AgentRunner::Open (DBHANDLE hDb, const char *pszAgentName, std::string &Err)
{
    STATUS error = NOERROR;

    Close();

    if ((NULLHANDLE == hDb) || IsNullStr (pszAgentName))
    {
        Err   = "No database or agent name";
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    m_hDb     = hDb;
    m_bOwnsDb = FALSE;

    error = OpenAgent (pszAgentName, Err);

Done:

    if (error)
        Close();

    return error;
}


/* The agent of the open database; closes everything again on an error. The run context is not made here: every run
 * has its own (it describes a single execution). */

STATUS AgentRunner::OpenAgent (const char *pszAgentName, std::string &Err)
{
    STATUS error       = NOERROR;
    NOTEID AgentNoteID = 0;

    m_AgentName = pszAgentName;

    error = NIFFindDesignNote (m_hDb, pszAgentName, NOTE_CLASS_FILTER, &AgentNoteID);

    if (error)
    {
        Err = std::string ("Agent [") + pszAgentName + "] not found: " + ErrorText (error);
        goto Done;
    }

    error = AgentOpen (m_hDb, AgentNoteID, &m_hAgent);

    if (error)
    {
        Err = std::string ("AgentOpen (") + pszAgentName + ") failed: " + ErrorText (error);
        m_hAgent = NULL;
        goto Done;
    }

Done:

    if (error)
        Close();

    return error;
}


STATUS AgentRunner::Run (std::string &Err)
{
    STATUS    error     = NOERROR;
    HAGENTCTX hAgentCtx = NULL;

    if (!IsOpen())
    {
        Err   = "Agent is not open";
        error = ERR_MISC_INVALID_ARGS;
        goto Done;
    }

    /* Flags 0 (AGENT_SECURITY_OFF): the agent runs unrestricted, the signer's agent privileges are not checked. That is
     * intended for the test agents.
     * Note for later: AGENT_SECURITY_ON makes the run context check the privileges of the agent signer (restricted or
     * unrestricted agent, can create databases, ...), as a normal user agent would be treated. Use it instead if a test
     * has to measure restricted agents. */
    error = AgentCreateRunContext (m_hAgent, NULL, AGENT_SECURITY_OFF, &hAgentCtx);

    if (error)
    {
        Err = std::string ("AgentCreateRunContext (") + m_AgentName + ") failed: " + ErrorText (error);
        hAgentCtx = NULL;
        goto Done;
    }

    if (m_dwTimeLimit)
    {
        error = AgentSetTimeExecutionLimit (hAgentCtx, m_dwTimeLimit);

        if (error)
        {
            Err = std::string ("AgentSetTimeExecutionLimit (") + m_AgentName + ") failed: " + ErrorText (error);
            goto Done;
        }
    }

    error = AgentRun (m_hAgent, hAgentCtx, NULLHANDLE, 0);

    if (error)
        Err = std::string ("AgentRun (") + m_AgentName + ") failed: " + ErrorText (error);

Done:

    if (hAgentCtx)
        AgentDestroyRunContext (hAgentCtx);

    return error;
}


void AgentRunner::Close()
{
    if (m_hAgent)
    {
        AgentClose (m_hAgent);
        m_hAgent = NULL;
    }

    if (m_hDb && m_bOwnsDb)
        NSFDbClose (m_hDb);

    m_hDb     = NULLHANDLE;
    m_bOwnsDb = FALSE;
}
