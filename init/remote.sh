#!app://shell.pxe
title --optional "Remote"
mount --optional --read-write host
namespace create
service start text app://textfs.pxe
session app://session.pxe --start-remote-services
