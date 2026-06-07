#!/bin/bash
set -euo pipefail
source /home/ricardo/ctrlX/ctrlx-automation-sdk/samples-cpp/TimeObserver/parts/flatbuffers/run/environment.sh
set -x
cp --archive --link --no-dereference . "/home/ricardo/ctrlX/ctrlx-automation-sdk/samples-cpp/TimeObserver/parts/flatbuffers/install"
