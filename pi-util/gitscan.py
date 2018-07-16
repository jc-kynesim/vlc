#!/usr/bin/env python

import os, string, subprocess, sys

# Local
import pipaths

def revdict():
    revdict = {'src':pipaths.src_commit}
    stuff = subprocess.check_output(["gclient", "revinfo"])
    for line in stuff.split("\n"):
        pathn = line.find(":")
        commitn = line.rfind("@")
        if pathn != -1 and commitn != -1 :
             revdict[line[:pathn]] = line[commitn+1:]
    return revdict

def basepath():
    cpath = os.getcwd()
    if not cpath.endswith("/src"):
        raise "CWD doesn't end with /src"

    return cpath[:-4]

def gitscan(args, nosrc = False, quiet=False):
    rv = 0

    oldcwd = os.getcwd()
    rdict = revdict()
    cpath = basepath()

    for p in pipaths.pipaths:
        if nosrc and p == "src":
            continue

        os.chdir(os.path.join(cpath, p))

        gitargs = [string.replace(string.replace(a, "{PATH}", p), "{BASE}", rdict[p]) for a in args]
        gitargs[0:0] = ["git"]

        if not quiet:
            print ">>>", p

        rv = subprocess.call(gitargs)
        if rv != 0:
            if not quiet:
                print "Git returned non-zero error code", rv, "\ncwd =", os.getcwd(), "\ncmd =", gitargs
            break

    os.chdir(oldcwd)
    return rv


if __name__ == '__main__':

    if len(sys.argv) < 2:
        print "Usage: gitscan [--gitscan-no-src] <git cmd>"
        print "  substitutes {PATH} and {BASE}"
        exit(0)

    nosrc = False

    if sys.argv[1] == "--gitscan-no-src":
        nosrc = True
        del sys.argv[1]

    gitscan(sys.argv[1:], nosrc)


