#!boot://shell.pxe
title --optional "Read-only"
mount --optional --read-only host
namespace create
service start text boot://textfs.pxe
session boot://session.pxe --start-services
