Import("env")
import os

build_number_file = os.path.join("include", "buildnumber.txt")

# Create file if it does not exist
if not os.path.exists(build_number_file):
    os.makedirs("include", exist_ok=True)
    with open(build_number_file, "w") as f:
        f.write("0")

# Read and increment
with open(build_number_file, "r") as f:
    try:
        build_number = int(f.read().strip())
    except ValueError:
        build_number = 0

build_number += 1

with open(build_number_file, "w") as f:
    f.write(str(build_number))

print(f"  HMS-GW-S3 build number: {build_number}")
env.Append(CPPDEFINES=[("BUILD_NUMBER", build_number)])
