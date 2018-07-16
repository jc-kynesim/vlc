#!/usr/bin/env python

import os, sys, string, subprocess

# Local
import gitscan, pipaths

def doscan(outfile = sys.stdout):
    revdict = gitscan.revdict()

    cpath = gitscan.basepath()

    for p in pipaths.pipaths:
        os.chdir(os.path.join(cpath, p))
        diff = subprocess.check_output(["git", "diff", revdict[p]])

        header = False
        lines = diff.split("\n")
        # Remove terminal blank line
        if lines[-1] == "":
            lines.pop()
        for line in lines:
            if line.startswith("diff --git "):
                header = True
            if header:
                line = string.replace(line, " a/", " a/" + p + "/")
                line = string.replace(line, " b/", " b/" + p + "/")
            if line.startswith("+++ "):
                header = False
            print >> outfile, line


if __name__ == '__main__':
    doscan()

