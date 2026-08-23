#!/usr/bin/env python3
"""mshtml: don't hand an app a NULL window when Gecko's document.open() fails.

SYMPTOM: Lightroom Classic hard-crashes the instant Help > About opens, killing the whole app
mid-session.

MEASURED CHAIN (2026-08-23, all of it instrumented, none of it inferred):
  * LrC's About box runs open() -> write() -> close() in a loop, 224 times.
  * The first 21 writes succeed. From the 22nd, every write fails with NS_ERROR_UNEXPECTED.
  * Gecko's WriteCommon (mozilla-esr52 dom/html/nsHTMLDocument.cpp, read not recalled) has exactly
    ONE source of that error:
        mTooDeepWriteRecursion = (mWriteLevel > NS_MAX_DOCUMENT_WRITE_DEPTH || mTooDeepWriteRecursion);
        NS_ENSURE_STATE(!mTooDeepWriteRecursion);
    NS_MAX_DOCUMENT_WRITE_DEPTH is 20, so writes 1-20 leave the level at 20, write 21 succeeds
    leaving 21, and write 22 onward fails. LrC's 21 successes match exactly.
  * The flag lives on the nsHTMLDocument, which LrC's open() REUSES (proven: identical
    dom_document/html_document across all 224 iterations), so the document stays poisoned.
  * On the 224th iteration Gecko refuses the open too. Wine returned E_FAIL with
    *pomWindowResult = NULL, LrC did not check the HRESULT, called Release() on NULL, and died.
    (The fault itself is a read at address 0x10: LrC calls Release() through the NULL
    window pointer it was handed.)

WHY RETURNING THE WINDOW IS THE RIGHT CALL, NOT A FUDGE:
  On Windows, document.open() on a live document does not fail this way -- there is no
  mTooDeepWriteRecursion in Trident. The E_FAIL is an artifact of OUR Gecko backend, so apps are
  not written against it, and LrC's missing check is invisible on Windows. Handing back the window
  (exactly as the success path does) restores the behaviour the app is entitled to expect.

⚠️ HONEST LIMITATION: this stops the CRASH, not the cause. The writes still fail, so the About box
will render empty rather than showing its text. Fixing that means stopping mWriteLevel from
leaking inside Gecko, which needs wine-gecko source and a Gecko rebuild. Crash -> empty dialog is
still a large win: the crash takes the user's whole Lightroom session with it.
"""
import os

p = os.path.expanduser("~/neutron-wine/_work/wine-tkg-git/wine-tkg-git/src/wine-git/dlls/mshtml/htmldoc.c")
s = open(p).read()
assert "neutron-open-recover" not in s, "already applied"

old = """    nsres = nsIDOMHTMLDocument_Open(This->html_document, NULL, NULL, NULL,
            get_context_from_document(This->dom_document), 0, &tmp);
    if(NS_FAILED(nsres)) {
        ERR("Open failed: %08lx\\n", nsres);
        return E_FAIL;
    }

    if(tmp)
        nsISupports_Release(tmp);"""

new = """    nsres = nsIDOMHTMLDocument_Open(This->html_document, NULL, NULL, NULL,
            get_context_from_document(This->dom_document), 0, &tmp);
    if(NS_FAILED(nsres)) {
        /* NEUTRON neutron-open-recover: hand back the window anyway, never a NULL out-param.
         *
         * Gecko can refuse document.open() for reasons Trident has no equivalent of -- in the
         * case this fixes, its per-document mTooDeepWriteRecursion latch, which trips once the
         * internal write depth passes 20 and is never cleared because the flag lives on the
         * nsHTMLDocument that open() reuses. Windows apps are therefore not written against this
         * failure: Lightroom Classic ignores the HRESULT entirely and calls Release() on the
         * out-param, so returning E_FAIL with *pomWindowResult = NULL crashes the whole
         * application the moment its About box opens.
         *
         * Returning the window matches what the success path below does and what Trident would
         * have done. The document content will be wrong -- the writes that poisoned it still
         * fail -- but a wrong document is recoverable and a dead process is not.
         *
         * ⚠️ This is deliberately a CRASH fix, not a cause fix. The cause is the leaked write
         * level inside Gecko; see wiki TODO P1k. */
        ERR("neutron-open-recover: Gecko Open failed (%08lx) -- returning the window rather than "
            "a NULL out-param, which apps that skip the HRESULT check dereference\\n", nsres);
        *pomWindowResult = (IDispatch*)&This->window->base.outer_window->base.IHTMLWindow2_iface;
        IHTMLWindow2_AddRef(&This->window->base.outer_window->base.IHTMLWindow2_iface);
        return S_OK;
    }

    if(tmp)
        nsISupports_Release(tmp);"""

assert s.count(old) == 1, "open-failure anchor"
open(p, "w").write(s.replace(old, new, 1))
print("mshtml/htmldoc.c: open() now returns the window instead of a NULL out-param on Gecko failure")
