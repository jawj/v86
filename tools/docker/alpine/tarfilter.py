#!/usr/bin/env python3

import tarfile
import io
import sys

INJECT_FILES = {
  'etc/hosts': b'127.0.0.1\tlocalhost\n::1\tlocalhost\n',
  'etc/network/interfaces': b'auto lo\niface lo inet loopback\n\nauto eth0\niface eth0 inet static\n  address 10.0.2.15\n  netmask 255.255.255.0\n  gateway 10.0.2.1\n',
  'etc/modprobe.d/no-usblp.conf': b'blacklist usblp\n',
}

DELETE_FILES = ['.dockerenv']

seen = set()

with tarfile.open(fileobj=sys.stdin.buffer, mode='r|') as inp, \
     tarfile.open(fileobj=sys.stdout.buffer, mode='w|') as out:
  for member in inp:
    if member.name in INJECT_FILES:
      data = INJECT_FILES[member.name]
      member.size = len(data)
      out.addfile(member, io.BytesIO(data))
      seen.add(member.name)
    elif member.name in DELETE_FILES:
      pass
    elif member.isreg():
      out.addfile(member, inp.extractfile(member))
    else:
      out.addfile(member)

  # inject any files that weren't already in the tar
  for name, data in INJECT_FILES.items():
    if name not in seen:
      info = tarfile.TarInfo(name=name)
      info.size = len(data)
      info.mode = 0o644
      out.addfile(info, io.BytesIO(data))
