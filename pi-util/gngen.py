#!/usr/bin/env python

import os, ast, fileinput, subprocess, sys

def docopy(name, vars):

    dest_dir = os.path.join("out", name)
    src_file = os.path.join("pi-util", "defargs_" + name + ".gn")

    # Ignore any errors making dir (in particular it already exists)
    try:
        os.makedirs(dest_dir)
    except:
        pass

    dargs = open(os.path.join(dest_dir, "args.gn"), "wt")
    dargs.write('# -- copied from: ' + src_file + '\n')

    for line in fileinput.input(src_file):
        dargs.write(line)

    dargs.write('# -- created by ' + sys.argv[0] + '\n')
    dargs.write('target_sysroot = "' + vars["target_sysroot"] + '"\n')
    dargs.write('google_api_key = "' + vars["google_api_key"] + '"\n')
    dargs.write('google_default_client_id = "' + vars["google_default_client_id"] + '"\n')
    dargs.write('google_default_client_secret = "' + vars["google_default_client_secret"] + '"\n')

    dargs.close()

    subprocess.check_call(["gn", "gen", dest_dir])


if __name__ == '__main__':
    gyp_vars = {}
    gypi = os.path.join(os.environ["HOME"], ".gyp", "include.gypi")
    if os.path.isfile(gypi):
        print "Importing from:", gypi
        gyps = open(gypi).read(-1)
        gyp_vars = ast.literal_eval(gyps)["variables"]

    gyp_vars["target_sysroot"] = os.path.abspath("build/linux/raspian_stretch_pi1-sysroot")

    docopy("armv6", gyp_vars)
    docopy("armv7", gyp_vars)



