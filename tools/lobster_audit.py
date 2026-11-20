#!/usr/bin/env python3
"""Check a resync replay of a SYNTHETIC LOBSTER-like export against the exporter's ground truth.

  tools/lobster_audit.py --audit FILE --messages FILE --orderbook FILE --summary summary.json [--json OUT]

The inputs are what `lobsim_export_lobster --audit FILE` and `lobsim_lobster --mode resync --out DIR`
write. Real LOBSTER data has no audit file, so this applies to synthetic exports only.

1. Reproduction: the files are replayed again here, independently of lobsim_lobster, by the same
   rules (seeding, the message table, seed attribution, resync with removals and tail trims, the
   burst check with its merges). Every counter, exact-row count, correction count and burst class
   of summary.json must come out equal.
2. Truth: the true book is rebuilt from the audit (every message, written or not). It must equal
   every orderbook row, and the executions of every burst must be what the true book's own
   price-time walk gives (consecutive fills of one order merged).
3. Attribution: every corrected level and removed level (every depth, level 1 included), every
   delete_size_mismatch, oversized_reduce and unknown_id, and every burst that is mismatched or
   excluded as unknown is traced to a root in the audit: a filtered message, or an order that rested
   below level L when the export window opened (the opening row shows only L levels). For a level,
   every order-level difference between the replayed book and the true book must have a root:
     stale order      a known order the truth no longer has          root: its delete was filtered
     cut order        a known order a resync trim or level removal   root: that trim or removal was
                      took quantity from                                   itself explained
     synthetic excess the synthetic orders hold more than the unseen root: filtered deletes there of
                      true orders                                          orders the replay held
                                                                           only in aggregate
     unseen order     a true order the replay does not know by id    root: submission filtered, the
                      (in aggregate a synthetic order may hold it)         opening depth, or a cut
An item without a root is UNEXPLAINED. Exit codes: 0 everything reproduced and explained,
1 a counter differs, the truth disagrees with the files, or an item is unexplained, 2 bad input.
"""
import argparse
import bisect
import collections
import csv
import json
import sys

BUY, SELL = 1, -1


class Book:
    """Price-time book: orders[oid] = [side, price, qty]; per side the prices best first."""

    def __init__(self):
        self.orders, self.levels, self.keys, self.next_id = {}, {BUY: {}, SELL: {}}, {BUY: [], SELL: []}, 1

    def prices(self, side):
        return [k if side == SELL else -k for k in self.keys[side]]

    def best(self, side):
        k = self.keys[side]
        return None if not k else (k[0] if side == SELL else -k[0])

    def insert(self, side, price, qty, oid=None):
        opp = self.best(-side)
        if opp is not None and (price >= opp if side == BUY else price <= opp):
            return None  # WouldCross
        if oid is None:
            oid, self.next_id = self.next_id, self.next_id + 1
        if price not in self.levels[side]:
            self.levels[side][price] = []
            bisect.insort(self.keys[side], price if side == SELL else -price)
        self.levels[side][price].append(oid)
        self.orders[oid] = [side, price, qty]
        return oid

    def remove(self, oid):
        side, price, _ = self.orders.pop(oid)
        lv = self.levels[side][price]
        lv.remove(oid)
        if not lv:
            del self.levels[side][price]
            k = self.keys[side]
            del k[bisect.bisect_left(k, price if side == SELL else -price)]

    def reduce(self, oid, qty):  # True when oversized; the order goes when qty >= remaining
        o = self.orders[oid]
        if qty >= o[2]:
            self.remove(oid)
            return qty > o[2]
        o[2] -= qty
        return False

    def queue(self, side, price):
        return list(self.levels[side].get(price, ()))

    def depth(self, side, n=None):
        return [(p, sum(self.orders[o][2] for o in self.levels[side][p])) for p in self.prices(side)[:n]]

    def preview(self, taker, limit, qty):
        fills, side = [], -taker
        for p in self.prices(side):
            if qty <= 0 or (p > limit if taker == BUY else p < limit):
                break
            for oid in self.levels[side][p]:
                if qty <= 0:
                    break
                take = min(qty, self.orders[oid][2])
                fills.append((oid, p, take))
                qty -= take
        return fills


class Truth:
    """The exporter's true book, advanced through the audit in step with the message file."""

    def __init__(self, rows):
        self.rows, self.pos, self.book = rows, 0, Book()
        self.origin = {}                  # id -> status of its latest submission
        self.born_line = {}               # id -> the line its latest submission was written at or before
        self.removed_by = {}              # id -> status of the message that last took it off the book
        self.filtered_reduction = set()   # ids with a filtered type 2 or 4
        self.filtered_deletes = collections.defaultdict(list)  # (side, price) -> [(line, id, size)]

    def advance(self, line, inclusive):
        """Apply the audit up to the written line `line` (included or not)."""
        while self.pos < len(self.rows):
            r = self.rows[self.pos]
            written = r['status'] in ('written', 'injected')
            if written and (r['line'] > line or (r['line'] == line and not inclusive)):
                return
            self.pos += 1
            self.apply(r)
            if written and r['line'] == line:
                return

    def apply(self, r):
        ty, oid, st = r['type'], r['id'], r['status']
        if ty == 1:
            if self.book.insert(r['direction'], r['price'], r['size'], oid) != oid:
                raise ValueError(f"audit row {r['seq']}: the true book refuses a submission")
            self.origin[oid], self.born_line[oid] = st, r['line']
        elif ty in (2, 3, 4):
            if oid not in self.book.orders:
                raise ValueError(f"audit row {r['seq']}: id {oid} is not resting in the true book")
            if ty == 3:
                self.book.remove(oid)
            elif self.book.reduce(oid, r['size']):
                raise ValueError(f"audit row {r['seq']}: oversized in the true book")
            if oid not in self.book.orders:
                self.removed_by[oid] = st
            if st == 'filtered':
                if ty == 3:
                    self.filtered_deletes[(r['direction'], r['price'])].append((r['line'], oid, r['size']))
                else:
                    self.filtered_reduction.add(oid)


class Audit:
    def __init__(self, msgs, rows, audit, summary):
        self.msgs, self.rows, self.L = msgs, rows, len(rows[0][0])
        self.truth = Truth(audit)
        self.book, self.by_ext, self.ext_of, self.syn = Book(), {}, {}, {}  # syn: oid -> creation line
        self.c = collections.Counter()
        self.exact_rows = [0] * self.L
        self.corrected, self.removed = [0] * self.L, [0] * self.L
        self.beyond = self.failed = self.resync_rows = self.divergent = self.crossed = 0
        m = summary['matching']
        self.depths = [d['trust_depth'] for d in m['by_trust_depth']]
        self.headline = m['headline_trust_depth']
        self.mc = {d: collections.Counter() for d in self.depths}
        self.bursts = self.burst_msgs = self.prev_exact = self.burst_left = self.mismatched = 0
        self.trigger = 'other'            # what the message of the current line did (for level-1 items)
        self.deep, self.seeded, self.deep_prices = set(), set(), set()
        self.cut = {}                     # ext id -> True if the trim/removal that cut it was explained
        self.syn_drawn = set()            # (side, price): synthetic quantity taken by explained trims or cut orders
        self.items = collections.defaultdict(collections.Counter)   # kind -> cause -> count
        self.unexplained = []             # (kind, line, detail)
        self.truth_errors = []
        self.deep_refs_before_window = 0  # references to deep orders before their price was ever inside the window
        self.deep_seen = set()            # (side, price) of deep orders once inside the published window

    # ------------------------------------------------------------------ attribution
    def origin(self, eid):
        """How the replay can know a true order: 'seeded' (inside a seed order of line 1: placed before
        the window at a published price, or message 1's own order), 'opening depth' (placed before the
        window below the published levels), or the status of its submission (written, filtered)."""
        o = self.truth.origin.get(eid)
        if eid in self.seeded and self.truth.born_line.get(eid, 0) <= 1:
            return 'seeded'
        return 'opening depth' if o == 'before_window' else o

    def differences(self, side, price):
        """(roots, problems) for every order-level difference at one level, replay vs truth."""
        roots, problems = collections.Counter(), []
        tq = {t: self.truth.book.orders[t][2] for t in self.truth.book.queue(side, price)}
        known, syn_q, since = set(), 0, None
        for oid in self.book.queue(side, price):
            q = self.book.orders[oid][2]
            if oid in self.syn:
                syn_q += q
                since = self.syn[oid] if since is None else min(since, self.syn[oid])
                continue
            eid = self.ext_of[oid]
            known.add(eid)
            if eid not in tq:
                if self.truth.removed_by.get(eid) == 'filtered':
                    roots['stale order: delete filtered'] += 1
                else:
                    problems.append(f'stale order {eid} not removed by a filtered message')
            elif q < tq[eid]:
                if self.cut.get(eid):
                    roots['cut order'] += 1
                else:
                    problems.append(f'order {eid}: {q} in the replay, {tq[eid]} in the truth, no explained cut')
            elif q > tq[eid]:
                if eid in self.truth.filtered_reduction:
                    roots['reduction filtered'] += 1
                else:
                    problems.append(f'order {eid}: {q} in the replay, {tq[eid]} in the truth, no filtered reduction')
        unseen_q, short_root = 0, False
        for t, q in tq.items():
            if t in known:
                continue
            unseen_q += q
            o = self.origin(t)
            if o == 'filtered':
                roots['submission filtered'] += 1
                short_root = True
            elif o == 'opening depth':
                roots['opening depth'] += 1
                short_root = True
            elif t in self.cut:
                if self.cut[t]:
                    roots['cut order'] += 1
                    short_root = True
                else:
                    problems.append(f'order {t} cut by an unexplained correction')
            elif o != 'seeded':
                problems.append(f'order {t} (submission {o}) unknown to the replay without a cause')
        if syn_q > unseen_q:
            fq = sum(sz for ln, i, sz in self.truth.filtered_deletes[(side, price)]
                     if ln >= since and i not in self.by_ext)
            if fq >= syn_q - unseen_q:
                roots['synthetic excess: deletes filtered'] += 1
            else:
                problems.append(f'synthetic orders hold {syn_q - unseen_q} more than the unseen orders, '
                                f'filtered deletes cover {fq}')
        elif syn_q < unseen_q:
            if short_root or (side, price) in self.syn_drawn:
                roots['synthetic short: unseen or cut orders'] += 1
            else:
                problems.append(f'synthetic orders hold {unseen_q - syn_q} less than the unseen orders')
        return roots, problems

    def primary(self, roots):
        for r in ('stale order: delete filtered', 'synthetic excess: deletes filtered', 'reduction filtered',
                  'cut order', 'submission filtered', 'opening depth', 'synthetic short: unseen or cut orders'):
            if roots[r]:
                return r
        return None

    def explain_levels(self, kind, line, side, prices):
        """Every order-level difference at these levels must have a root; at least one must exist.
        Returns the primary root, or None after recording the item as unexplained."""
        roots, problems = collections.Counter(), []
        for p in sorted(prices):
            r, pr = self.differences(side, p)
            roots.update(r)
            problems += [f"{'ask' if side == SELL else 'bid'} {p}: {x}" for x in pr]
        cause = self.primary(roots)
        if problems or cause is None:
            self.unexplained.append((kind, line, '; '.join(problems[:3]) or 'no order-level difference'))
            return None
        self.items[kind][cause] += 1
        return cause

    def explain_order(self, kind, line, eid, side, price, unknown):
        """An unknown id (unknown=True), or a known order whose remaining disagrees with the message."""
        if kind != 'excluded_unknown':
            self.trigger = kind + (' (a cut order)' if eid in self.cut else '')
        if eid in self.cut:
            cause = 'cut order' if self.cut[eid] else None
        elif not unknown:
            cause = 'reduction filtered' if eid in self.truth.filtered_reduction else None
        else:
            o = self.origin(eid)
            cause = {'filtered': 'submission filtered', 'opening depth': 'opening depth'}.get(o)
            if o == 'seeded' and (side, price) in self.syn_drawn:
                cause = 'seed synthetic drawn by cut orders or trims'
        if cause is None:
            self.unexplained.append((kind, line, f'order {eid} (submission {self.truth.origin.get(eid)})'))
        else:
            self.items[kind][cause] += 1

    # ------------------------------------------------------------------ replay
    def forget_if_gone(self, oid):
        if oid in self.book.orders:
            return
        self.syn.pop(oid, None)
        e = self.ext_of.pop(oid, None)
        if e is not None:
            del self.by_ext[e]

    def apply(self, line, m):
        _, ty, eid, sz, pr, d = m
        c, b = self.c, self.book
        if ty == 1:
            if eid in self.by_ext:
                c['dup_id'] += 1
            elif (oid := b.insert(d, pr, sz)) is None:
                c['submit_would_cross'] += 1
            else:
                self.by_ext[eid], self.ext_of[oid] = oid, eid
                self.cut.pop(eid, None)
                c['submitted'] += 1
            return
        if ty in (5, 6, 7):
            c[{5: 'hidden_exec', 6: 'cross_trade', 7: 'halt'}[ty]] += 1
            return
        oid = self.by_ext.get(eid)
        if oid is not None:
            side, price, rem = b.orders[oid]
            if side != d:
                c['direction_mismatch'] += 1
            elif price != pr:
                c['price_mismatch'] += 1
            else:
                if ty == 3:
                    if sz != rem:
                        c['delete_size_mismatch'] += 1
                        self.explain_order('delete_size_mismatch', line, eid, d, pr, False)
                    b.remove(oid)
                    c['deleted'] += 1
                else:
                    if b.reduce(oid, sz):
                        c['oversized_reduce'] += 1
                        self.explain_order('oversized_reduce', line, eid, d, pr, False)
                    c['cancelled' if ty == 2 else 'executed'] += 1
                self.forget_if_gone(oid)
            return
        for o2 in b.queue(d, pr):
            if o2 in self.syn and b.orders[o2][2] >= sz:
                b.reduce(o2, sz)
                self.forget_if_gone(o2)
                c['seed_attributed'] += 1
                if self.cut.get(eid):
                    self.syn_drawn.add((d, pr))
                return
        c['unknown_id'] += 1
        self.explain_order('unknown_id', line, eid, d, pr, True)

    def compare(self, line, row):
        L, b = self.L, self.book
        ours = {SELL: b.depth(SELL, L), BUY: b.depth(BUY, L)}
        orc = {SELL: row[0], BUY: row[1]}
        if ours[SELL] and ours[BUY] and ours[BUY][0][0] >= ours[SELL][0][0]:
            self.crossed += 1

        def same(side, i):
            o, r = ours[side], orc[side][i]
            return r[1] == 0 if i >= len(o) else (r[1] != 0 and o[i] == r)

        exact = 0
        while exact < L and same(SELL, exact) and same(BUY, exact):
            exact += 1
        for i in range(exact):
            self.exact_rows[i] += 1
        self.prev_exact = exact
        if exact < L:
            self.divergent += 1
            self.resync(line, row)

    def resync(self, line, row):
        b, L = self.book, self.L
        plans = [(SELL, row[0], b.depth(SELL)), (BUY, row[1], b.depth(BUY))]
        changed = False
        for side, orc, before in plans:
            omap = {p: q for p, q in orc if q}
            bound = orc[-1][0] if len(omap) == L else None
            for rank, (p, _) in enumerate(before, 1):
                if bound is not None and (p > bound if side == SELL else p < bound):
                    break
                if p in omap:
                    continue
                cause = self.explain_levels('removed_level', line, side, [p])
                if rank == 1:
                    self.level_1(line, cause)
                ok = cause is not None
                for oid in b.queue(side, p):
                    if oid in self.ext_of:
                        self.cut[self.ext_of[oid]] = ok
                    b.remove(oid)
                    self.forget_if_gone(oid)
                if rank <= L:
                    self.removed[rank - 1] += 1
                else:
                    self.beyond += 1
                changed = True
        for side, orc, before in plans:
            have_at = dict(before)
            for d, (p, want) in enumerate(orc):
                if want == 0 or have_at.get(p, 0) == want:
                    continue
                have = have_at.get(p, 0)
                cause = self.explain_levels('corrected_level', line, side, [p])
                if d == 0:
                    self.level_1(line, cause)
                ok = cause is not None
                if have < want:
                    oid = b.insert(side, p, want - have)
                    if oid is None:
                        self.failed += 1
                    else:
                        self.syn[oid] = line
                else:
                    self.trim(side, p, have - want, ok)
                self.corrected[d] += 1
                changed = True
        if changed:
            self.resync_rows += 1

    def level_1(self, line, cause):
        if cause is None:
            self.unexplained.append(('level_1', line, 'see the corrected_level or removed_level item of this line'))
        else:
            self.items['level_1'][cause] += 1
            self.items['level_1 (the message of the line)'][self.trigger] += 1

    def trim(self, side, price, excess, explained):
        b = self.book
        for oid in reversed(b.queue(side, price)):
            if excess <= 0:
                break
            take = min(excess, b.orders[oid][2])
            if oid in self.syn:
                if explained:
                    self.syn_drawn.add((side, price))
            else:
                eid = self.ext_of[oid]
                self.cut[eid] = self.cut.get(eid, True) and explained
            b.reduce(oid, take)
            self.forget_if_gone(oid)
            excess -= take

    def check_burst(self, i, line):
        msgs, b = self.msgs, self.book
        head = msgs[i]
        burst = [i]
        while burst[-1] + 1 < len(msgs):
            m = msgs[burst[-1] + 1]
            if m[1] != 4 or m[0] != head[0] or m[5] != head[5]:
                break
            burst.append(burst[-1] + 1)
        self.burst_left = len(burst) - 1
        self.bursts += 1
        self.burst_msgs += len(burst)
        resting = head[5]
        taken = collections.defaultdict(int)
        data, last_syn, last_known, unknown_at, prices, qty, limit = [], 0, 0, None, set(), 0, head[4]
        for k in burst:
            _, _, eid, sz, pr, _ = msgs[k]
            prices.add(pr)
            qty += sz
            limit = max(limit, pr) if resting == SELL else min(limit, pr)
            oid = self.by_ext.get(eid)
            if oid is not None and taken[oid] < b.orders[oid][2]:
                if b.orders[oid][0] == resting and b.orders[oid][1] == pr:
                    taken[oid] += sz
                if last_known == oid and data[-1][1] == eid and data[-1][2] == pr:
                    data[-1] = (False, eid, pr, data[-1][3] + sz)
                else:
                    data.append((False, eid, pr, sz))
                last_syn, last_known = 0, oid
                continue
            last_known = 0
            for o2 in b.queue(resting, pr):
                if o2 not in self.syn or b.orders[o2][2] - taken[o2] < sz:
                    continue
                taken[o2] += sz
                if last_syn == o2:
                    data[-1] = (True, 0, pr, data[-1][3] + sz)
                else:
                    data.append((True, 0, pr, sz))
                last_syn = o2
                break
            else:
                if unknown_at is None:
                    unknown_at = k
        taker = -resting
        # the truth: the data must be the true book's own walk (consecutive fills of one order merged)
        walk = [(t, p, q) for t, p, q in self.truth.book.preview(taker, limit, qty)]
        got = []
        for k in burst:
            _, _, eid, sz, pr, _ = msgs[k]
            if got and got[-1][0] == eid:
                got[-1] = (eid, pr, got[-1][2] + sz)
            else:
                got.append((eid, pr, sz))
        if walk != got:
            self.truth_errors.append(f'line {line}: the burst {got} is not the true book walk {walk}')
        if unknown_at is not None:
            cls = 'excluded_unknown'
            m = msgs[unknown_at]
            self.explain_order('excluded_unknown', unknown_at + 1, m[2], m[5], m[4], True)
        else:
            predicted = []
            for oid, p, q in b.preview(taker, limit, qty):
                anon = oid in self.syn or oid not in self.ext_of
                predicted.append((anon, 0 if anon else self.ext_of[oid], p, q))
            cls = compare_fills(data, predicted, taker)
            if cls not in ('exact', 'exact_via_synthetic'):  # every comparison that disagrees, scored or not
                self.mismatched += 1
                self.explain_levels('mismatched_burst', line, resting, {f[2] for f in data + predicted})
        for td in self.depths:
            cl = cls
            if cl != 'excluded_unknown' and not (self.prev_exact >= td and td >= len(prices) + 1):
                cl = 'excluded_untrusted'
            self.mc[td][cl] += 1
            if cl.startswith('price_skip'):
                self.mc[td]['price_skip'] += 1

    def run(self):
        L = self.L
        for i, m in enumerate(self.msgs):
            line = i + 1
            self.c['messages'] += 1
            self.c[f'type_{m[1]}'] += 1
            if line == 1:
                self.truth.advance(1, True)
                self.c['consumed_by_seed'] += 1
                for side, lv in ((SELL, self.rows[0][0]), (BUY, self.rows[0][1])):
                    for p, q in lv:
                        if q:
                            oid = self.book.insert(side, p, q)
                            if oid is None:
                                self.c['seed_rejected'] += 1
                            else:
                                self.syn[oid] = 1
                                self.c['seed_orders'] += 1
                for side in (SELL, BUY):
                    for p, _ in self.truth.book.depth(side, L):
                        self.seeded.update(self.truth.book.queue(side, p))
                self.deep = {t for t, o in self.truth.origin.items()
                             if o == 'before_window' and t in self.truth.book.orders and t not in self.seeded}
                self.deep_prices = {tuple(self.truth.book.orders[t][:2]) for t in self.deep}
            else:
                self.trigger = 'other'
                if m[1] in (2, 3, 4) and m[2] in self.deep and (m[5], m[4]) not in self.deep_seen:
                    self.deep_refs_before_window += 1
                self.truth.advance(line, False)
                if self.burst_left > 0:
                    self.burst_left -= 1
                elif m[1] == 4:
                    self.check_burst(i, line)
                self.apply(line, m)
                self.truth.advance(line, True)
            asks, bids = self.rows[i]
            if self.truth.book.depth(SELL, L) != [x for x in asks if x[1]] or \
                    self.truth.book.depth(BUY, L) != [x for x in bids if x[1]]:
                self.truth_errors.append(f'line {line}: the true book is not the orderbook row')
            self.compare(line, self.rows[i])
            for side, price in self.deep_prices - self.deep_seen:  # deep prices once inside the published window
                ne = [p for p, q in (asks if side == SELL else bids) if q]
                if len(ne) < L or (price <= ne[-1] if side == SELL else price >= ne[-1]):
                    self.deep_seen.add((side, price))


def compare_fills(data, predicted, taker):
    syn, n = False, min(len(data), len(predicted))
    for k in range(n):
        d, p = data[k], predicted[k]
        syn = syn or d[0] or p[0]
        if d[2] != p[2]:
            return 'price_skip_better' if (p[2] < d[2] if taker == BUY else p[2] > d[2]) else 'price_skip_worse'
        if not (d[0] == p[0] and (d[0] or d[1] == p[1])):
            return 'queue_order'
        if d[3] != p[3]:
            return 'size_split'
    if len(data) != len(predicted):
        return 'length'
    return 'exact_via_synthetic' if syn else 'exact'


def read_inputs(args):
    def ints(text, n, what):
        f = text.rstrip('\r\n').split(',')
        if len(f) != n:
            raise ValueError(f'{what}: {len(f)} fields, {n} expected')
        return f

    msgs, rows = [], []
    with open(args.messages) as fm, open(args.orderbook) as fo:
        for k, (lm, lo) in enumerate(zip(fm, fo), 1):
            t, ty, oid, sz, pr, d = ints(lm, 6, f'message line {k}')
            sec, _, frac = t.partition('.')
            if not sec.isdigit() or not frac.isdigit() or len(frac) > 9:
                raise ValueError(f'message line {k}: time {t}')
            msgs.append((int(sec) * 10**9 + int(frac.ljust(9, '0')), int(ty), int(oid), int(sz), int(pr), int(d)))
            v = [int(x) for x in lo.rstrip('\r\n').split(',')]
            if len(v) % 4 or (rows and len(v) != 4 * len(rows[0][0])):
                raise ValueError(f'orderbook line {k}: {len(v)} fields')
            rows.append(([(v[i], v[i + 1]) for i in range(0, len(v), 4)], [(v[i + 2], v[i + 3]) for i in range(0, len(v), 4)]))
    if len(msgs) != sum(1 for _ in open(args.messages)) or len(rows) != sum(1 for _ in open(args.orderbook)):
        raise ValueError('the message and orderbook files have different lengths')
    audit = []
    with open(args.audit) as fa:
        for r in csv.DictReader(fa):
            audit.append({k: (v if k == 'status' else int(v)) for k, v in r.items()})
    with open(args.summary) as fs:
        summary = json.load(fs)
    if summary.get('mode') != 'resync' or not summary.get('completed') or summary.get('levels') != len(rows[0][0]):
        raise ValueError('the summary must be a completed resync replay of these files')
    return msgs, rows, audit, summary


def reproduce(a, s):
    """Every reproduced number that differs from summary.json, as (name, summary, ours)."""
    bad = []
    ours = dict(a.c, crossed_state=a.crossed)
    for k, v in s['counters'].items():
        if ours.get(k, 0) != v:
            bad.append((f'counters.{k}', v, ours.get(k, 0)))
    r = s['resync']
    for name, want, got in (('lines', s['lines'], len(a.msgs)), ('rows_compared', s['rows_compared'], len(a.rows)),
                            ('divergent_rows', s['divergent_rows'], a.divergent),
                            ('exact_rows', [x['exact'] for x in s['exact_rows']], a.exact_rows),
                            ('resync.rows', r['rows'], a.resync_rows),
                            ('resync.corrected_by_level', r['corrected_by_level'], a.corrected),
                            ('resync.removed_by_rank', r['removed_by_rank'], a.removed),
                            ('resync.removed_beyond_level_l', r['removed_beyond_level_l'], a.beyond),
                            ('resync.failed', r['failed'], a.failed),
                            ('matching.bursts', s['matching']['bursts'], a.bursts),
                            ('matching.burst_messages', s['matching']['burst_messages'], a.burst_msgs)):
        if want != got:
            bad.append((name, want, got))
    for t in s['matching']['by_trust_depth']:
        mc = a.mc[t['trust_depth']]
        for k in ('exact', 'exact_via_synthetic', 'queue_order', 'price_skip', 'price_skip_better', 'price_skip_worse',
                  'size_split', 'length', 'excluded_unknown', 'excluded_untrusted'):
            if t[k] != mc[k]:
                bad.append((f"matching.T{t['trust_depth']}.{k}", t[k], mc[k]))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    for name in ('audit', 'messages', 'orderbook', 'summary'):
        ap.add_argument('--' + name, required=True)
    ap.add_argument('--json', help='write the attribution as JSON here')
    args = ap.parse_args()
    try:
        msgs, rows, audit, summary = read_inputs(args)
        a = Audit(msgs, rows, audit, summary)
        a.run()
    except (OSError, ValueError, KeyError) as e:
        print(f'lobster_audit: {e}', file=sys.stderr)
        return 2
    bad = reproduce(a, summary)
    print('SYNTHETIC export audit: ' + args.messages)
    print(f'reproduction of summary.json: {"all numbers equal" if not bad else f"{len(bad)} DIFFER"}')
    for name, want, got in bad:
        print(f'  {name}: summary {want}, reproduced {got}')
    print(f'truth vs files: {"every row and every burst agree" if not a.truth_errors else f"{len(a.truth_errors)} DISAGREE"}')
    for e in a.truth_errors[:10]:
        print('  ' + e)
    c, r, m = summary['counters'], summary['resync'], summary['matching']
    hd = next(t for t in m['by_trust_depth'] if t['trust_depth'] == m['headline_trust_depth'])
    totals = {'level_1': r['corrected_by_level'][0] + r['removed_by_rank'][0],
              'corrected_level': sum(r['corrected_by_level']),
              'removed_level': sum(r['removed_by_rank']) + r['removed_beyond_level_l'],
              'delete_size_mismatch': c['delete_size_mismatch'], 'oversized_reduce': c['oversized_reduce'],
              'unknown_id': c['unknown_id'], 'excluded_unknown': hd['excluded_unknown'],
              'mismatched_burst': a.mismatched}
    unexplained = collections.Counter(k for k, _, _ in a.unexplained)
    report = {'reproduced': not bad, 'truth_agrees': not a.truth_errors, 'unexplained': len(a.unexplained),
              'deep_references_before_their_level_was_published': a.deep_refs_before_window, 'items': {}}
    print('attribution (every item needs a root; the counts per cause add up to the total):')
    totals['level_1 (the message of the line)'] = totals['level_1']
    for kind in ('level_1', 'level_1 (the message of the line)', 'corrected_level', 'removed_level',
                 'delete_size_mismatch', 'oversized_reduce', 'unknown_id', 'mismatched_burst', 'excluded_unknown'):
        got = a.items[kind]
        n = sum(got.values()) + unexplained[kind]
        print(f'  {kind}: {n} (to explain: {totals[kind]})')
        if n != totals[kind]:
            a.unexplained.append((kind, 0, f'{n} items attributed, {totals[kind]} to explain'))
        for cause, v in got.most_common():
            print(f'    {v:8d}  {cause}')
        if unexplained[kind]:
            print(f'    {unexplained[kind]:8d}  UNEXPLAINED')
        report['items'][kind] = dict(got, total=n, unexplained=unexplained[kind])
    for kind, line, detail in a.unexplained[:20]:
        print(f'  UNEXPLAINED {kind} at line {line}: {detail}')
    print(f'references to orders resting below level {a.L} at the window start, made before their price was '
          f'ever inside the published window: {a.deep_refs_before_window}')
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(report, f, indent=1, sort_keys=True)
    ok = not bad and not a.truth_errors and not a.unexplained
    print('RESULT: ' + ('every number reproduced, every discrepancy explained' if ok else 'FAILED'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
