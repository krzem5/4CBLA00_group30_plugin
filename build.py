############################################################################
# Copyright (c) Krzesimir Hyżyk - All Rights Reserved                      #
# Unauthorized copying of this file, via any medium is strictly prohibited #
# Proprietary and confidential                                             #
# Created on 08/10/2024 by Krzesimir Hyżyk                                 #
############################################################################

import os
import subprocess
import sys



def _get_source_files(*directories):
	for directory in directories:
		for root,_,files in os.walk(directory):
			for file in files:
				if (file.endswith(".c")):
					yield os.path.join(root,file)



if (not os.path.exists("build")):
	os.mkdir("build")
if ("--release" in sys.argv):
	object_files=[]
	error=False
	for file in _get_source_files("src"):
		object_file="build/"+file.replace("/","$")+".o"
		object_files.append(object_file)
		if (subprocess.run(["gcc","-Wall","-Werror","-Ofast","-ggdb","-ffast-math","-c",file,"-o",object_file,"-Isrc/include","-DNULL=((void*)0)","-D_GNU_SOURCE"]).returncode!=0):
			error=True
	if (error or subprocess.run(["gcc","-Ofast","-ggdb","-ffast-math","-o","build/plugin"]+object_files+["-lm"]).returncode!=0):
		sys.exit(1)
else:
	object_files=[]
	error=False
	for file in _get_source_files("src"):
		object_file="build/"+file.replace("/","$")+".o"
		object_files.append(object_file)
		if (subprocess.run(["gcc","-Wall","-Werror","-O0","-g","-c",file,"-o",object_file,"-Isrc/include","-DNULL=((void*)0)","-D_GNU_SOURCE"]).returncode!=0):
			error=True
	if (error or subprocess.run(["gcc","-O0","-o","build/plugin"]+object_files+["-lm"]).returncode!=0):
		sys.exit(1)
if ("--run" in sys.argv):
	subprocess.run(["build/plugin","<marc_input_file>","<host_file_directory>","<vm_file_directory>","<vm_ssh_username>","<vm_ssh_password>","<vm_ssh_address>","<min_force>","<max_force>","<divisions>"])
