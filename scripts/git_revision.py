import os

git_hash = os.environ.get("DDNET_GIT_SHORTREV_HASH")
if not git_hash:
	git_hash = "4e4269396b97d06879c11ae3b9696c3dc1a06084"
if git_hash is not None:
	definition = f'"{git_hash}"'
else:
	definition = "0"
print("#include <game/version.h>")
print(f"const char *GIT_SHORTREV_HASH = {definition};")
