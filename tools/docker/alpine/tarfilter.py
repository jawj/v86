#!/usr/bin/env python3

import tarfile
import io
import sys

with tarfile.open(fileobj=sys.stdin.buffer, mode='r|') as inp, \
     tarfile.open(fileobj=sys.stdout.buffer, mode='w|') as out:
    for member in inp:
        if member.name == 'etc/hosts':
            data = b'127.0.0.1\tlocalhost\n::1\tlocalhost\n'
            member.size = len(data)
            out.addfile(member, io.BytesIO(data))
        elif member.name == '.dockerenv':
            pass
        elif member.isreg():
            out.addfile(member, inp.extractfile(member))
        else:
            out.addfile(member)
