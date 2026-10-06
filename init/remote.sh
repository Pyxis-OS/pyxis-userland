#!boot://shell.pxe
title --optional "Remote"
mount --optional --read-write host
namespace create
service start text boot://textfs.pxe
session boot://session.pxe --start-remote-services
