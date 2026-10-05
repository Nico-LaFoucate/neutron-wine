#!/usr/bin/env python3
"""Catch patch-ordering bugs in the Neutron patch set BEFORE they cost a build.

    check-patch-order.py [patches-dir]        # default: ../patches next to this script

WHY THIS EXISTS
    wine-tkg applies every *.mypatch in the userpatches directory in the order the shell
    enumerates them -- which is the SYSTEM LOCALE's collation, not ASCII. Locale collation
    ignores punctuation at the primary level, so names that look correctly ordered in
    ASCII can apply in the opposite order in practice:

        neutron-dcomp-zzz-d2dui-bridge      <- defines the code
        neutron-dcomp-zzz2-direct-publish   <- edits that code

    In C collation the bridge sorts first ('-' 0x2d < '2' 0x32) and all is well. Under
    en_US.UTF-8 the comparison is effectively "zzzd2dui..." vs "zzz2direct...", where '2'
    sorts before 'd' -- so the edit applied BEFORE the thing it edits. That cost a build
    on 2026-07-30.

    WHAT MADE IT DANGEROUS: only ONE hunk failed. The rest applied with fuzz 1-2 at
    offsets of -89 to -163 lines against the wrong base. `patch` is happy to place a hunk
    anywhere it fuzzily matches, so a slightly luckier misapply yields a SILENTLY CORRUPT
    build rather than a clean failure. Ordering must be verified, not assumed.

WHAT IT CHECKS
    ERROR -- a patch declares an ordering contract in its header ("applies AFTER X") and
        does not actually sort after X under every collation. This is the precise check:
        it is what the 2026-07-30 failure would have tripped, and it has no false
        positives, because the contract is stated by the patch itself.

    WARN -- a file is touched by 2+ patches whose relative order differs between
        collations AND whose hunks land in overlapping regions of the file. Divergent
        order alone is NOT reported: four such cases have existed in this patch set for
        many releases and build fine, because the patches touch unrelated parts of the
        file. Reporting those as errors would just train everyone to ignore the tool.

Exit status is non-zero only on ERROR, so it can gate a build without crying wolf.
"""
import collections
import functools
import locale
import os
import re
import sys


def sort_keys():
    """(name, keyfunc) for each collation we care about."""
    out = [("C", lambda s: s.encode())]
    try:
        locale.setlocale(locale.LC_COLLATE, '')
        out.append((locale.setlocale(locale.LC_COLLATE), locale.strxfrm))
    except locale.Error:
        pass
    return out


def files_touched(path):
    out = set()
    for line in open(path, errors='replace'):
        if line.startswith('+++ b/'):
            out.add(line[6:].strip())
        elif line.startswith('+++ ') and '/dev/null' not in line:
            out.add(line[4:].split('\t')[0].strip().lstrip('b/'))
    return out


def hunk_ranges(path, target):
    """Approximate line ranges a patch edits in one file, in the ORIGINAL file's numbering."""
    out, cur = [], None
    for line in open(path, errors='replace'):
        if line.startswith('+++ '):
            name = line[4:].split('\t')[0].strip()
            cur = name[2:] if name.startswith('b/') else name
        elif line.startswith('@@') and cur == target:
            m = re.match(r'@@ -(\d+)(?:,(\d+))? ', line)
            if m:
                start = int(m.group(1))
                out.append((start, start + int(m.group(2) or 1)))
    return out


def overlaps(a, b, slack=60):
    """Do two sets of ranges land near each other? Slack absorbs the offset drift that
    earlier patches introduce -- exact adjacency is not knowable without applying them."""
    return any(not (e1 + slack < s2 or e2 + slack < s1) for s1, e1 in a for s2, e2 in b)


def declared_after(path):
    """Patch names this one claims to apply after, from its header comment."""
    head = []
    for line in open(path, errors='replace'):
        if not line.startswith('#'):
            break
        head.append(line)
    text = ''.join(head)
    names = set()
    for m in re.finditer(r'(?:applies )?AFTER\s+([A-Za-z0-9_.-]+)', text, re.I):
        n = m.group(1).rstrip('.,')
        if n.startswith('neutron') or n.endswith('.mypatch'):
            names.add(n if n.endswith('.mypatch') else n + '.mypatch')
    return names


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'patches')
    names = [f for f in os.listdir(d) if f.endswith('.mypatch')]
    if not names:
        print("no patches found in", d)
        return 2

    collations = sort_keys()
    orders = {c: sorted(names, key=k) for c, k in collations}
    print("patches: %d in %s" % (len(names), d))
    print("collations compared: %s" % ", ".join(c for c, _ in collations))

    touched = {n: files_touched(os.path.join(d, n)) for n in names}
    by_file = collections.defaultdict(list)
    for n in names:
        for f in touched[n]:
            by_file[f].append(n)

    errors, warns = [], []

    # ERROR: a declared "applies AFTER X" contract that does not hold.
    for n in names:
        for dep in declared_after(os.path.join(d, n)):
            if dep not in names:
                errors.append("%s declares AFTER %s, which is not in the patch set" % (n, dep))
                continue
            for c, _ in collations:
                seq = orders[c]
                if seq.index(n) < seq.index(dep):
                    errors.append("%s must apply AFTER %s but sorts BEFORE it under %s"
                                  % (n, dep, c))

    # WARN: divergent order AND overlapping edit regions.
    for f, plist in sorted(by_file.items()):
        if len(plist) < 2:
            continue
        seqs = {c: [n for n in orders[c] if n in plist] for c, _ in collations}
        if len({tuple(v) for v in seqs.values()}) == 1:
            continue
        ranges = {n: hunk_ranges(os.path.join(d, n), f) for n in plist}
        risky = [(a, b) for i, a in enumerate(plist) for b in plist[i + 1:]
                 if overlaps(ranges[a], ranges[b])
                 and any(orders[c].index(a) < orders[c].index(b) for c, _ in collations)
                 and any(orders[c].index(a) > orders[c].index(b) for c, _ in collations)]
        if risky:
            warns.append("%s -- order differs between collations AND hunks are close:" % f)
            for a, b in risky:
                warns.append("      %s  <->  %s" % (a, b))

    print()
    for w in warns:
        print("WARN  " + w)
    if warns:
        print()
    if errors:
        print("!! %d ERROR(S)" % len(errors))
        for e in errors:
            print("  " + e)
        print("\nFix by renaming so the order holds under EVERY collation listed above.")
        print("A name that sorts last under both is one whose distinguishing character is a")
        print("LETTER, not punctuation (e.g. 'zzzz-' rather than 'zzz2-'): locale collation")
        print("ignores punctuation at the primary level, ASCII does not.")
        return 1

    multi = sum(1 for f, p in by_file.items() if len(p) > 1)
    print("OK: no declared ordering contract is violated under any collation.")
    print("    (%d files are touched by more than one patch; %d flagged for review above.)"
          % (multi, len(warns)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
