#!/usr/bin/env python3
"""mshtml: deliver the end-of-parse notifications from the message loop, not from inside close().

MEASURED (2026-08-23, from Lightroom's own +mshtml trace, lrc-about2.log):
  * LrC's 224 open->write->close cycles are NESTED, not sequential. The stack address of open()'s
    arguments falls by exactly 3,136 bytes on every call -- 0x10DF40, 0x10D200, 0x10C4C0 ...
    0x055480 -- 739 KB of stack consumed and never unwound.
  * The lines immediately before each recursive open() are:
        HTMLDocument_close -> nsDocumentObserver_EndLoad -> run_end_load -> parse_complete
        -> IDocObjectService_FireNavigateComplete2 -> (LrC's handler) -> HTMLDocument_open
    So LrC starts each cycle from inside the previous cycle's close().
  * Because the cycles nest, Gecko's per-document write depth climbs one level per cycle instead
    of returning to zero. Past NS_MAX_DOCUMENT_WRITE_DEPTH (20) its mTooDeepWriteRecursion latch
    trips and stays tripped: writes 1-21 succeed, 22-224 all fail with NS_ERROR_UNEXPECTED. That
    is why the About box renders empty.

WHY THIS IS A WINE BUG, NOT AN LrC BUG:
  Native MSHTML parses HTML on a separate thread and delivers DocumentComplete /
  NavigateComplete2 / the readystate property change from the message loop, so the host's handler
  always runs on a fresh stack after close() has returned. Wine's run_end_load() calls
  parse_complete() inline and says so itself:

      /*
       * This should be done in the worker thread that parses HTML,
       * but we don't have such thread (Gecko parses HTML for us).
       */

  We do not have a parser thread either, but we do have the task queue that already carries
  set_progress_proc and set_downloading_proc for exactly this reason. Posting parse_complete
  through it restores the native ordering: the notification is delivered from the message loop,
  the host's re-entrant call starts at depth 0, and nothing accumulates.

This is the CAUSE fix for the About box. The earlier neutron-open-recover patch stops the crash
that the failure caused; this stops the failure.
"""
import os

p = os.path.expanduser("~/neutron-wine/_work/wine-tkg-git/wine-tkg-git/src/wine-git/dlls/mshtml/mutation.c")
s = open(p).read()
assert "neutron-parse-complete-async" not in s, "already applied"

old = """static nsresult run_end_load(HTMLDocumentNode *This, nsISupports *arg1, nsISupports *arg2)
{"""

new = """/* NEUTRON neutron-parse-complete-async: run parse_complete() off the message loop.
 *
 * Native MSHTML parses on its own thread and fires DocumentComplete / NavigateComplete2 / the
 * readystate change from the message loop, so a host handler that drives the control further --
 * Lightroom Classic's About box calls document.open() again -- always starts on a fresh stack.
 * Calling parse_complete() inline from close() instead lets the host re-enter without ever
 * unwinding: LrC nests 224 open/write/close cycles that way, 739 KB of stack, and Gecko's
 * document write depth climbs one level per cycle until its mTooDeepWriteRecursion latch trips
 * at depth 20 and every subsequent write fails. Posting it is what native does. */
static void parse_complete_proc(task_t *_task)
{
    docobj_task_t *task = (docobj_task_t*)_task;

    parse_complete(task->doc);
}

static void parse_complete_destr(task_t *_task)
{
    docobj_task_t *task = (docobj_task_t*)_task;

    IUnknown_Release(task->doc->outer_unk);
}

static void async_parse_complete(HTMLDocumentObj *doc)
{
    docobj_task_t *task;

    if(!(task = malloc(sizeof(*task)))) {
        parse_complete(doc);
        return;
    }

    task->doc = doc;
    IUnknown_AddRef(doc->outer_unk);
    push_task(&task->header, parse_complete_proc, parse_complete_destr, doc->task_magic);
}

static nsresult run_end_load(HTMLDocumentNode *This, nsISupports *arg1, nsISupports *arg2)
{"""

assert s.count(old) == 1, "run_end_load anchor"
s = s.replace(old, new, 1)

old2 = """        IUnknown_AddRef(doc_obj->outer_unk);
        parse_complete(doc_obj);
        IUnknown_Release(doc_obj->outer_unk);"""
new2 = """        async_parse_complete(doc_obj);"""
assert s.count(old2) == 1, "parse_complete call anchor"
s = s.replace(old2, new2, 1)

old3 = """    if(This->window == window && window->base.outer_window) {
        window->dom_interactive_time = get_time_stamp();
        set_ready_state(window->base.outer_window, READYSTATE_INTERACTIVE);
    }"""
new3 = """    if(This->window == window && window->base.outer_window) {
        window->dom_interactive_time = get_time_stamp();
        /* NEUTRON neutron-parse-complete-async: the readystate change is the other callback the
         * host can see from in here, and a host that drives the control from it re-enters just as
         * hard. Wine already has the lock for exactly this -- navigate.c and xmlhttprequest.c use
         * it around synchronous work -- and it posts the notification instead of calling it, while
         * still updating window->readystate immediately. */
        window->base.outer_window->readystate_locked++;
        set_ready_state(window->base.outer_window, READYSTATE_INTERACTIVE);
        window->base.outer_window->readystate_locked--;
    }"""
assert s.count(old3) == 1, "set_ready_state anchor"
s = s.replace(old3, new3, 1)

open(p, "w").write(s)
print("mshtml/mutation.c: parse_complete() and the readystate notification now run from the task queue")
