#!app://shell.pxe
title --optional "Read-only"
mount --optional --read-only host
namespace create
service start text app://textfs.pxe
session app://session.pxe --start-services
