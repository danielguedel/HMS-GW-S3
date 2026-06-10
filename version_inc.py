Import("env")
import os

build_number_file = "include/buildnumber.txt"

if not os.path.exists(build_number_file):
    with open(build_number_file, "w") as f:
        f.write("1")

with open(build_number_file, "r") as f:
    build_number = int(f.read().strip() or "0")

build_number += 1

with open(build_number_file, "w") as f:
    f.write(str(build_number))

print(f"Build number: {build_number}")
env.Append(CPPDEFINES=[("BUILD_NUMBER", build_number)])
