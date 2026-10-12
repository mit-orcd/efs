#!/bin/bash
exec timeout "${EFS_SSH_TIMEOUT:-15}" ssh -o BatchMode=yes -o ConnectTimeout=8 "erbmi1@$1" "$2"
