#!/bin/sh
if [ -f config-local.sh ]; then
    . ./config-local.sh 
else
    [ -f /usr/etc/chilli/defaults ] && . /usr/etc/chilli/defaults
    [ -f /usr/etc/chilli/config ]   && . /usr/etc/chilli/config
fi
