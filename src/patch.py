#!/usr/bin/env python
from __future__ import print_function
import zmq
import sys
import json

def save(filename, contents):
    with open(filename + '_versions.json', 'a+') as f:
        # Read the old versions.
        f.seek(0)
        try:
            versions = json.load(f)
        except ValueError:
            versions = []
        # Clear the file.
        f.seek(0)
        f.truncate()
        # Add the new version to the file.
        versions.append(contents)
        json.dump(versions, f)

def patch(filename):
    ctx = zmq.Context()
    sock = ctx.socket(zmq.REQ)
    sock.connect('tcp://127.0.0.1:5555')
    with open(filename) as f:
        string = f.read()
        save(filename, string)
        sock.send(string.encode('utf-8'))
        print(sock.recv().decode('utf-8'))

if __name__ == '__main__':
    if len(sys.argv) > 1:
        patch(sys.argv[1])
