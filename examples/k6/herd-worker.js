// herd-worker.js - an nshtestherd worker written as a k6 script.
//
// Every k6 virtual user (VU) is one worker:
//   - it registers once and receives its own test_id and account,
//   - it polls POST /status, reports its state and acknowledges the last command it applied,
//   - it applies each command_id exactly once (run / pause / stop / idle),
//   - while "running" it does one HTTP request per poll to TARGET_URL
//     (default: the coordinator's own /status, so the example needs no other system).
//
// Put your real test steps into doTest(). The account of this worker is in `me`
// (me.shortname, me.password, me.internetaddress, ...).
//
// Things to know:
//   - Stop the workers before restarting the coordinator. A restarted coordinator is a fresh herd and may
//     hand the same test_id to another client, so a 404 is NOT a reliable restart signal. On a 404 this
//     worker treats its allocation as lost, logs it and stops its load; it never registers again.
//   - A stop command makes a worker report "done". When k6 simply ends (DURATION reached, Ctrl+C) the workers
//     are gone without a final report: the coordinator keeps their last reported state and their accounts
//     stay reserved until the coordinator is restarted (no recovery by design).
//   - Several k6 instances (distributed run): give every instance its own RUN_ID, for example the host name.
//
// Run:  k6 run examples/k6/herd-worker.js           (see examples/k6/README.md)

import http from 'k6/http';
import { check, sleep } from 'k6';

const HERD         = __ENV.HERD_URL || 'http://127.0.0.1:8788';
const TARGET       = __ENV.TARGET_URL || `${HERD}/status`;
const POLL_SECONDS = Number(__ENV.POLL_SECONDS || 2);

// Part of every worker's registration key (k6-<RUN_ID>-<VU>). VU numbers start at 1 in every k6 instance, so
// two instances must never share a RUN_ID or their workers would share allocations. Set RUN_ID explicitly for
// distributed runs; the random fallback is only safe because each k6 instance generates its own.
const RUN_ID       = __ENV.RUN_ID || `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 8)}`;

// Ask for JSON replies: res.json() then gives the flat fields directly.
const JSON_HEADERS = { Accept: 'application/json' };

export const options = {
    scenarios: {
        workers: {
            executor: 'constant-vus',
            vus: Number(__ENV.VUS || 5),
            duration: __ENV.DURATION || '5m',
        },
    },
};

// Module-level variables are per VU in k6: each worker has its own copy.
let me          = null;     // registration reply (account and test_id)
let state       = 'idle';   // what this worker reports
let applied     = 0;        // last command_id applied, sent back as ack_command_id
let resumeState = 'idle';   // state to return to after a timed pause
let pauseUntil  = 0;        // Date.now() value at which a timed pause ends
let lost        = false;    // the coordinator no longer knows this worker: no more load


function register()
{
    // The key is generated once per worker and repeated on every retry, so a retry returns the same account.
    const res = http.post(
        `${HERD}/register`,
        { request_key: `k6-${RUN_ID}-${__VU}` },
        { headers: JSON_HEADERS, tags: { name: 'herd_register' } });

    if (res.status === 200 || res.status === 201)
    {
        me = res.json();
        console.log(`worker ${__VU}: registered as ${me.shortname} (test_id ${me.test_id})`);
        return;
    }

    // 409 pool_exhausted: every account is taken. 409 no_accounts_loaded: start the coordinator with accounts.
    console.error(`worker ${__VU}: registration failed (${res.status}) ${res.body}`);
}


// Reports the current state and acknowledgement; returns the current instruction, or null.
function report(message)
{
    const body = { test_id: me.test_id, state: state, ack_command_id: applied };

    if (message)
    {
        body.message = message;
    }

    const res = http.post(`${HERD}/status`, body, { headers: JSON_HEADERS, tags: { name: 'herd_status' } });

    if (res.status === 404)
    {
        // The allocation is lost (coordinator restarted, or a wrong test_id). Do not register again: a restarted
        // coordinator may reuse this test_id for another client, so the worker must not carry on.
        console.error(`worker ${__VU}: allocation lost (test_id ${me.test_id} unknown), stopping this worker's load`);
        lost = true;
        return null;
    }

    if (res.status !== 200)
    {
        console.error(`worker ${__VU}: status report failed (${res.status})`);
        return null;
    }

    return res.json();
}


function apply(instruction)
{
    applied = instruction.command_id;

    switch (instruction.command)
    {
        case 'idle':
            state = 'idle';
            break;

        case 'run':
            state = 'running';
            break;

        case 'pause':
            if (state !== 'paused')
            {
                resumeState = state;
            }

            state      = 'paused';
            pauseUntil = Date.now() + instruction.pause_seconds * 1000;
            break;

        case 'stop':
            state = 'stopping';
            report('stopping');
            state = 'done';
            report('stopped by command');
            return;
    }

    report(`applied command ${instruction.command_id}: ${instruction.command} ${instruction.job}`);
}


// The actual test step. Replace this with your own requests; use me.shortname / me.password to log in.
function doTest()
{
    const res = http.get(TARGET, { tags: { name: 'target' } });

    check(res, { 'target answers 200': (r) => r.status === 200 });
}


export default function ()
{
    if (lost)
    {
        // Allocation lost: this worker does nothing any more (see report()).
        sleep(POLL_SECONDS);
        return;
    }

    if (me === null)
    {
        register();

        if (me === null)
        {
            sleep(POLL_SECONDS);
            return;
        }
    }

    if (state === 'done')
    {
        // After a stop command the worker only idles until k6 ends (DURATION or Ctrl+C).
        sleep(POLL_SECONDS);
        return;
    }

    // A timed pause ends on the worker's own clock: return to the state before the pause.
    if (state === 'paused' && Date.now() >= pauseUntil)
    {
        state = resumeState;
    }

    const instruction = report();

    if (lost)
    {
        // The report just found out the allocation is lost: no load in this iteration either.
        sleep(POLL_SECONDS);
        return;
    }

    // Apply each command_id once: the same id seen again changes nothing (no restart, no new pause timer).
    if (instruction && instruction.command_id > applied)
    {
        apply(instruction);
    }

    if (state === 'running')
    {
        doTest();
    }

    sleep(POLL_SECONDS);
}
