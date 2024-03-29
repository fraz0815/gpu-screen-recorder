#!/bin/sh -e

script_dir=$(dirname "$0")
cd "$script_dir"

[ $(id -u) -ne 0 ] && echo "You need root privileges to run the install script" && exit 1

./build.sh
strip gsr-kms-server
strip gpu-screen-recorder

install -Dm755 "gsr-kms-server" "/usr/bin/gsr-kms-server"
install -Dm755 "gpu-screen-recorder" "/usr/bin/gpu-screen-recorder"
if [ -d "/usr/lib/systemd/user" ]; then
    install -Dm644 "extra/gpu-screen-recorder.service" "/usr/lib/systemd/user/gpu-screen-recorder.service"
fi
# Not necessary, but removes the password prompt when trying to record a monitor on amd/intel or nvidia wayland
setcap cap_sys_admin+ep /usr/bin/gsr-kms-server
# Not necessary, but allows use of EGL_CONTEXT_PRIORITY_LEVEL_IMG which allows gpu screen recorder to run without being limited to game fps under heavy load
setcap cap_sys_nice+ep /usr/bin/gpu-screen-recorder

echo "Successfully installed gpu-screen-recorder"
