#!/usr/bin/env python3
"""Self-test of tools/lobster_audit.py on tiny hand-written SYNTHETIC exports (upgrade 2b, D-214).

  lobster_audit_test.py <tools/lobster_audit.py> <lobsim_lobster> <work dir>

1. A filtered delete leaves a stale order that resync removes at rank 1: explained, exit 0.
2. The same order book with every message written: nothing to explain, exit 0.
3. A hand-edited message file (a delete names the wrong order; the audit says it was written):
   an unknown id and a level-1 correction with no root, exit 1.
4. A hand-edited summary.json (unknown_id + 1): the reproduction fails, exit 1.
"""
import json
import os
import shutil
import subprocess
import sys

audit_tool, lobster, work = sys.argv[1:4]
# The work folder is deleted: only ever the one ctest passes (CMakeLists.txt), never a folder a
# mistyped argument names.
if os.path.basename(os.path.normpath(work)) != 'lobster_audit_self_test':
    sys.exit('lobster_audit_test.py: the work folder must be named lobster_audit_self_test, not ' + work)
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)
failures = []


def case(name, levels, written, audit_rows, rows, edit=None):
    """written: message lines; audit_rows: (type, id, size, price, direction, status) in order."""
    d = os.path.join(work, name)
    os.makedirs(d)
    stem = os.path.join(d, 'TEST_2012-06-21_34200000_57600000_')
    msg, book = f'{stem}message_{levels}.csv', f'{stem}orderbook_{levels}.csv'
    with open(msg, 'w') as f:
        f.write(''.join(f'34200.{k + 1:09d},{m}\n' for k, m in enumerate(written)))
    with open(book, 'w') as f:
        f.write(''.join(r + '\n' for r in rows))
    with open(os.path.join(d, 'audit.csv'), 'w') as f:
        f.write('seq,sim_time_ns,type,id,size,price,direction,status,line\n')
        line = 0
        for seq, (ty, oid, size, price, direction, status) in enumerate(audit_rows):
            line += status == 'written'
            f.write(f'{seq},{seq + 1},{ty},{oid},{size},{price},{direction},{status},{line}\n')
    if edit:
        with open(msg) as f:
            text = f.read()
        with open(msg, 'w') as f:
            f.write(text.replace(*edit))
    r = subprocess.run([lobster, '--messages', msg, '--orderbook', book, '--mode', 'resync', '--out',
                        os.path.join(d, 'replay')], capture_output=True, text=True)
    if r.returncode != 0:
        failures.append(f'{name}: lobsim_lobster exited with {r.returncode}: {r.stderr}')
    return d, msg, book


def audit(name, d, msg, book, want_rc, want_text, summary=None):
    r = subprocess.run([sys.executable, audit_tool, '--audit', os.path.join(d, 'audit.csv'), '--messages', msg,
                        '--orderbook', book, '--summary', summary or os.path.join(d, 'replay', 'summary.json')],
                       capture_output=True, text=True)
    print(f'--- {name} (exit {r.returncode})\n{r.stdout}{r.stderr}')
    if r.returncode != want_rc or want_text not in r.stdout:
        failures.append(f'{name}: exit {r.returncode} (expected {want_rc}), "{want_text}" '
                        f'{"found" if want_text in r.stdout else "missing"}')


# 1. L = 1. Order 7 rests at bid 99 while it is the only bid; order 1 at 100 pushes 99 out of the
# window, and the delete of 7 is filtered. The delete of 1 leaves the published bid side empty, so
# resync removes the replay's stale level 99 at rank 1: explained by the filtered delete.
d, msg, book = case('filtered_delete', 1,
                    ['1,2,10,200,-1', '1,7,5,99,1', '1,1,10,100,1', '3,1,10,100,1'],
                    [(1, 2, 10, 200, -1, 'written'), (1, 7, 5, 99, 1, 'written'), (1, 1, 10, 100, 1, 'written'),
                     (3, 7, 5, 99, 1, 'filtered'), (3, 1, 10, 100, 1, 'written')],
                    ['200,10,0,0', '200,10,99,5', '200,10,100,10', '200,10,0,0'])
audit('filtered_delete', d, msg, book, 0, 'stale order: delete filtered')
with open(os.path.join(d, 'replay', 'summary.json')) as f:
    if json.load(f)['resync']['removed_by_rank'] != [1]:
        failures.append('filtered_delete: the replay did not remove the stale level at rank 1')

# 2 and 3. L = 2, every message written. Orders 5 and 6 (5 lots each) join order 1 at bid 100 and
# are deleted. In case 3 the message file's first delete names 6 instead of 5 (the level's size is
# the same, so the row still agrees); the second delete of 6 is then an unknown id and the level
# is 5 lots too big: a level-1 correction that no filtered message explains.
written = ['1,2,10,200,-1', '1,1,10,100,1', '1,5,5,100,1', '1,6,5,100,1', '3,5,5,100,1', '3,6,5,100,1']
truth = [(1, 2, 10, 200, -1, 'written'), (1, 1, 10, 100, 1, 'written'), (1, 5, 5, 100, 1, 'written'),
         (1, 6, 5, 100, 1, 'written'), (3, 5, 5, 100, 1, 'written'), (3, 6, 5, 100, 1, 'written')]
rows = ['200,10,0,0,0,0,0,0', '200,10,100,10,0,0,0,0', '200,10,100,15,0,0,0,0', '200,10,100,20,0,0,0,0',
        '200,10,100,15,0,0,0,0', '200,10,100,10,0,0,0,0']
d, msg, book = case('clean', 2, written, truth, rows)
audit('clean', d, msg, book, 0, 'every number reproduced, every discrepancy explained')
d, msg, book = case('edited_message', 2, written, truth, rows,
                    edit=('34200.000000005,3,5,5,100,1', '34200.000000005,3,6,5,100,1'))
audit('edited_message', d, msg, book, 1, 'UNEXPLAINED unknown_id at line 6')

# 4. The clean replay with a hand-edited summary.
d = os.path.join(work, 'clean')
with open(os.path.join(d, 'replay', 'summary.json')) as f:
    s = json.load(f)
s['counters']['unknown_id'] += 1
edited = os.path.join(work, 'edited_summary.json')
with open(edited, 'w') as f:
    json.dump(s, f)
stem = os.path.join(d, 'TEST_2012-06-21_34200000_57600000_')
audit('edited_summary', d, stem + 'message_2.csv', stem + 'orderbook_2.csv', 1, 'counters.unknown_id: summary 1',
      summary=edited)

if failures:
    print('FAILED:\n  ' + '\n  '.join(failures))
    sys.exit(1)
shutil.rmtree(work)
print('lobster_audit self-test passed')
