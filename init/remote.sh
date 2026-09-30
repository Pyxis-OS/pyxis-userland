#!app://shell.pxe
title --optional "Remote"
mount --optional --read-write host
namespace create
service start text app://textfs.pxe
service start http app://httpfs.pxe
service start --optional --read-only https app://httpfs.pxe --https
session app://session.pxe --remote-server 2323
