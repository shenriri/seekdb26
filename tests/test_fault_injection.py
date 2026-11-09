#!/usr/bin/env python3
"""Process-SIGKILL fault injection; independently restart from the same /data."""
import os
import subprocess
import sys
import tempfile

exe=sys.argv[1]

def invoke(data,sql,failpoint=None):
    env=os.environ.copy()
    env.pop('MVDB_TEST_FAILPOINT',None)
    if failpoint:env['MVDB_TEST_FAILPOINT']=failpoint
    p=subprocess.run([exe,'--data-dir',data],input=sql+'\n.exit\n',text=True,
                     capture_output=True,encoding='utf-8',env=env,timeout=15)
    return p.returncode,p.stdout
with tempfile.TemporaryDirectory(prefix='mvdb_kill_') as data:
    code,output=invoke(data,"CREATE TABLE x (id BIGINT PRIMARY KEY, v VECTOR(2))")
    assert code==0 and output=="OK\n",(code,output)
    code,output=invoke(data,"INSERT INTO x VALUES (1,[0,0])")
    assert code==0 and output=="OK\n",(code,output)

    # Broken WAL tail: not committed, must disappear on restart.
    code,_=invoke(data,"INSERT INTO x VALUES (2,[0,0])",'mid_wal_write')
    assert code<0,(code,_)
    code,output=invoke(data,"SELECT id FROM x ORDER BY id")
    assert code==0 and output=="ROWS\t1\nROW\t1\nEND\n",(code,output)

    # A fully synchronized commit is recovered even if no success reply was returned.
    code,_=invoke(data,"INSERT INTO x VALUES (3,[0,0])",'after_sync_before_apply')
    assert code<0,(code,_)
    code,output=invoke(data,"SELECT id FROM x ORDER BY id")
    assert code==0 and output=="ROWS\t2\nROW\t1\nROW\t3\nEND\n",(code,output)

    # Crash after COW page fsync, before manifest publish. WAL replays it.
    code,_=invoke(data,"INSERT INTO x VALUES (4,[0,0])",'after_index_before_manifest')
    assert code<0,(code,_)
    code,_=invoke(data,"SELECT id FROM x",'recovery_after_replay')
    assert code<0,(code,_)
    code,output=invoke(data,"SELECT id FROM x ORDER BY id")
    assert code==0 and output=="ROWS\t3\nROW\t1\nROW\t3\nROW\t4\nEND\n",(code,output)

    # Crash before index pages are published, after writing heap data.
    code,_=invoke(data,"INSERT INTO x VALUES (6,[2,0])",'after_heap_before_index')
    assert code<0,(code,_)
    code,output=invoke(data,"SELECT id FROM x ORDER BY id")
    assert code==0 and output=="ROWS\t4\nROW\t1\nROW\t3\nROW\t4\nROW\t6\nEND\n",(code,output)

    # A manifest that was published before SIGKILL must not replay its commit twice.
    code,_=invoke(data,"INSERT INTO x VALUES (5,[1,0])",'after_manifest_published')
    assert code<0,(code,_)
    code,output=invoke(data,"SELECT id FROM x ORDER BY id")
    assert code==0 and output=="ROWS\t5\nROW\t1\nROW\t3\nROW\t4\nROW\t5\nROW\t6\nEND\n",(code,output)

    # Check record ordering after further commits, including an UPDATE.
    code,output=invoke(data,"UPDATE x SET v = [1,1] WHERE id = 3")
    assert code==0 and output=="OK\n",(code,output)
    code,output=invoke(data,"SELECT id FROM x ORDER BY L2_DISTANCE(v, [0,0])")
    assert code==0 and output=="ROWS\t5\nROW\t1\nROW\t4\nROW\t5\nROW\t3\nROW\t6\nEND\n",(code,output)
print('PASS: torn-WAL, post-sync, interrupted COW, recovery SIGKILL, and post-manifest idempotence')
