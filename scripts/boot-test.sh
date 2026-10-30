#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
export PATH=$HOME/.cargo/bin:/usr/bin:$PATH
exec python3 "$(dirname "$0")/boot-test.py" "$@"
