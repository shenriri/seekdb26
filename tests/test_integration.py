#!/usr/bin/env python3
"""Tests for the provisional newline SQL interface (NOT official mysqltest protocol)."""
import os
import pathlib
import subprocess
import sys
import tempfile

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minivectordb"

class Client:
    def __init__(self, path, directory):
        self.proc = subprocess.Popen([path, "--data-dir", str(directory)], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, encoding="utf-8", bufsize=1)
    def sql(self, text):
        assert "\n" not in text
        assert self.proc.poll() is None, "database exited unexpectedly"
        self.proc.stdin.write(text+"\n")
        self.proc.stdin.flush()
        first = self.proc.stdout.readline().rstrip("\n")
        assert first, "no response for: " + text
        if first.startswith("ERR\t"):
            return ("ERR", first[4:])
        if first == "OK":
            return ("OK", None)
        assert first.startswith("ROWS\t"), first
        n = int(first.split("\t")[1])
        rows=[]
        for _ in range(n):
            line=self.proc.stdout.readline().rstrip("\n")
            assert line.startswith("ROW"), line
            rows.append(line.split("\t")[1:])
        assert self.proc.stdout.readline().strip()=="END"
        return ("ROWS", rows)
    def close(self):
        self.proc.stdin.write(".exit\n")
        self.proc.stdin.flush()
        self.proc.communicate(timeout=10)
        assert self.proc.returncode == 0
    def kill(self):
        self.proc.kill()
        self.proc.communicate(timeout=10)

with tempfile.TemporaryDirectory(prefix="mvdb_test_") as d:
    c=Client(BIN,d)
    ok=lambda sql: (lambda x: assert_ok(x,sql))(c.sql(sql))
    def query(sql):
        result=c.sql(sql)
        assert result[0]=="ROWS", (sql,result)
        return result[1]
    def err(sql, expected):
        result=c.sql(sql)
        assert result[0]=="ERR" and expected in result[1], (sql,result)
    def assert_ok(result, sql):
        assert result[0]=="OK", (sql,result)

    ok("CREATE TABLE items (id BIGINT PRIMARY KEY, tag VARCHAR(4), embedding VECTOR(2))")
    ok("INSERT INTO items VALUES (3, '蓝色', [2, 0]), (1, '红色', [0, 1])")
    assert query("SELECT * FROM items") == [["1","红色","[0,1]"],["3","蓝色","[2,0]"]]
    assert query("SELECT id FROM items WHERE id >= 2") == [["3"]]
    assert query("SELECT id FROM items WHERE id >= 2 AND tag = '蓝色'") == [["3"]]
    assert query("SELECT id FROM items WHERE id >= 2 AND tag = '红色'") == []
    assert query("SELECT id FROM items WHERE tag >= '红色'") == [["1"],["3"]]
    assert query("SELECT id FROM items ORDER BY id DESC LIMIT 1") == [["3"]]
    assert query("SELECT id, L2_DISTANCE(embedding, '[0,0]') AS score FROM items ORDER BY score LIMIT 2")==[["1","1"],["3","2"]]
    assert query("SELECT id FROM items WHERE tag = '红色' ORDER BY L2_DISTANCE(embedding, [0,0]) LIMIT 10")==[["1"]]
    err("INSERT INTO items VALUES (4, 'ok', [0,0]), (1, 'bad', [0,0])", "DUPLICATE_PRIMARY_KEY")
    assert query("SELECT id FROM items WHERE id=4")==[]
    err("INSERT INTO items VALUES (4,'abcde',[0,0])", "VARCHAR_LENGTH_EXCEEDED")
    err("INSERT INTO items VALUES (4,'ok',[1,2,3])", "VECTOR_DIM_MISMATCH")
    err("INSERT INTO items VALUES (4,'ok',[1e40,0])", "INVALID_VECTOR_VALUE")
    err("INSERT INTO items VALUES (9223372036854775808,'ok',[0,0])", "INVALID_BIGINT")
    err("UPDATE items SET id=7 WHERE id=1", "PRIMARY_KEY_UPDATE_NOT_SUPPORTED")
    err("CREATE TABLE bad (id BIGINT, z VECTOR(2))", "REQUIRES_ONE_BIGINT_PRIMARY_KEY")
    err("SELECT x FROM items", "UNKNOWN_COLUMN")
    err("SELECT id FROM items WHERE embedding = '[0,1]'", "UNSUPPORTED_VECTOR_COMPARISON")
    ok("CREATE VECTOR INDEX idx ON items(embedding)")
    err("CREATE VECTOR INDEX idx2 ON items(embedding)", "VECTOR_INDEX_ALREADY_EXISTS")
    ok("BEGIN")
    ok("INSERT INTO items (id,embedding,tag) VALUES (4,[0,0],'z')")
    ok("UPDATE items SET tag='changed' WHERE id=1".replace("changed","变"))
    ok("DELETE FROM items WHERE id=3")
    assert query("SELECT id FROM items ORDER BY id")==[["1"],["4"]]
    assert query("SELECT id FROM items ORDER BY L2_DISTANCE(embedding,[0,0]) LIMIT 2")==[["4"],["1"]]
    err("INSERT INTO items VALUES (5,'a',[0,0]), (4,'b',[0,0])", "DUPLICATE_PRIMARY_KEY")
    assert query("SELECT id FROM items WHERE id=5")==[]
    ok("ROLLBACK")
    assert query("SELECT id FROM items ORDER BY id")==[["1"],["3"]]
    ok("BEGIN")
    ok("INSERT INTO items VALUES (9,'ok',[0,0])")
    err("INSERT INTO items VALUES (9,'bad',[0,0])", "DUPLICATE_PRIMARY_KEY")
    ok("COMMIT")
    assert query("SELECT id FROM items ORDER BY id")==[["1"],["3"],["9"]]
    ok("INSERT INTO items VALUES (-5,'neg',[3,4])")
    ok("UPDATE items SET embedding = [0,0] WHERE id=3")
    assert query("SELECT id FROM items ORDER BY L2_DISTANCE(embedding,[0,0]) LIMIT 2")==[["3"],["9"]]
    c.close()

    # Crash recovery: committed writes survive, uncommitted writes disappear.
    c=Client(BIN,d)
    assert c.sql("SELECT id FROM items ORDER BY id")[1]==[["-5"],["1"],["3"],["9"]]
    assert c.sql("BEGIN")[0]=="OK"
    assert c.sql("INSERT INTO items VALUES (42,'temp',[1,1])")[0]=="OK"
    c.kill()
    c=Client(BIN,d)
    assert c.sql("SELECT id FROM items ORDER BY id")[1]==[["-5"],["1"],["3"],["9"]]
    assert c.sql("INSERT INTO items VALUES (10,'new',[1,1])")[0]=="OK"
    c.kill()
    c=Client(BIN,d)
    assert c.sql("SELECT id FROM items ORDER BY id")[1]==[["-5"],["1"],["3"],["9"],["10"]]
    c.close()

    # Simulate an incomplete, torn WAL tail. Valid earlier commits must remain.
    with open(pathlib.Path(d)/"wal.mvdb","ab") as out:out.write(b"\x4d\x44\x42")
    c=Client(BIN,d)
    assert c.sql("SELECT id FROM items WHERE id=10")[1]==[["10"]]
    c.close()

    # Duplicate PK across two rows of the same INSERT statement is atomic.
    c=Client(BIN,d)
    r=c.sql("INSERT INTO items VALUES (20,'a',[0,0]),(20,'b',[1,1])")
    assert r[0]=="ERR" and "DUPLICATE_PRIMARY_KEY" in r[1]
    assert c.sql("SELECT id FROM items WHERE id=20")[1]==[]
    c.close()

print("PASS: SQL, Unicode, float32 validation, transactional overlay, statement atomicity, WAL recovery, torn-tail handling")
